import asyncio
import secrets
import time
from contextlib import asynccontextmanager
from typing import Optional

from fastapi import FastAPI, Header, HTTPException, Request, Response, status
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse

from app.config import settings
from app.influx import influx_service
from app.link_health import link_health_tracker
from app.logger import audit_logger, logger
from app.models import ErrorResponse, IngestResponse, ReadingPayload

START_TIME = time.time()


async def staleness_monitor_task():
    """Background task running every 15s to update staleness in InfluxDB if no packets arrive."""
    while True:
        try:
            await asyncio.sleep(15)
            now = time.time()
            for state in link_health_tracker.get_all_states():
                gap = state.current_gap_seconds(now)
                if state.is_stale(now):
                    influx_service.write_stale_update(state.node_id, gap_seconds=gap, stale=True)
        except asyncio.CancelledError:
            break
        except Exception as e:
            logger.error(f"Error in staleness monitor task: {e}")


@asynccontextmanager
async def lifespan(app: FastAPI):
    logger.info("SecureLoRa Ingest Service starting up...")
    monitor_task = asyncio.create_task(staleness_monitor_task())
    yield
    logger.info("SecureLoRa Ingest Service shutting down...")
    monitor_task.cancel()
    try:
        await monitor_task
    except asyncio.CancelledError:
        pass
    influx_service.close()


app = FastAPI(
    title="SecureLoRa Ingest Service",
    description="High-reliability ingest service for SecureLoRa gateway readings.",
    version="1.0.0",
    lifespan=lifespan,
)


@app.exception_handler(RequestValidationError)
async def validation_exception_handler(request: Request, exc: RequestValidationError):
    """
    Ensure missing fields or malformed JSON strictly return HTTP 400
    per Section 2 of server_guide.
    """
    client_ip = request.client.host if request.client else "unknown"
    error_msg = f"Validation error: {exc.errors()}"
    audit_logger.log_event(
        client_ip=client_ip,
        status_code=status.HTTP_400_BAD_REQUEST,
        duration_ms=0.0,
        server_rx_time=time.time(),
        endpoint=request.url.path,
        payload=None,
        error=error_msg,
    )
    return JSONResponse(
        status_code=status.HTTP_400_BAD_REQUEST,
        content={"error": "Bad JSON or missing required fields", "details": exc.errors()},
    )


@app.exception_handler(HTTPException)
async def http_exception_handler(request: Request, exc: HTTPException):
    client_ip = request.client.host if request.client else "unknown"
    audit_logger.log_event(
        client_ip=client_ip,
        status_code=exc.status_code,
        duration_ms=0.0,
        server_rx_time=time.time(),
        endpoint=request.url.path,
        payload=None,
        error=str(exc.detail),
    )
    return JSONResponse(
        status_code=exc.status_code,
        content={"error": exc.detail},
    )


def verify_bearer_token(authorization: Optional[str] = Header(None)) -> None:
    """
    Verifies Bearer token.
    Section 2: '401: wrong or missing token. The token is write-only.'
    """
    expected_token = settings.gateway_api_token
    # If a token is configured on the server, require and validate it
    if expected_token:
        if not authorization:
            logger.warning("Rejected POST request: missing Authorization header")
            raise HTTPException(
                status_code=status.HTTP_401_UNAUTHORIZED,
                detail="Missing Authorization header. Expected Bearer token.",
                headers={"WWW-Authenticate": "Bearer"},
            )
        parts = authorization.split(" ")
        if len(parts) != 2 or parts[0].lower() != "bearer":
            logger.warning("Rejected POST request: malformed Authorization header")
            raise HTTPException(
                status_code=status.HTTP_401_UNAUTHORIZED,
                detail="Invalid Authorization header format. Expected 'Bearer <token>'.",
                headers={"WWW-Authenticate": "Bearer"},
            )
        token = parts[1]
        # Use constant-time comparison to prevent timing attacks
        if not secrets.compare_digest(token, expected_token):
            logger.warning("Rejected POST request: invalid Bearer token")
            raise HTTPException(
                status_code=status.HTTP_401_UNAUTHORIZED,
                detail="Invalid Bearer token.",
                headers={"WWW-Authenticate": "Bearer"},
            )


@app.post(
    "/api/readings",
    response_model=IngestResponse,
    status_code=status.HTTP_200_OK,
    responses={
        400: {"model": ErrorResponse, "description": "Bad JSON or missing required fields"},
        401: {"model": ErrorResponse, "description": "Unauthorized: invalid or missing bearer token"},
    },
)
async def ingest_reading(
    payload: ReadingPayload,
    request: Request,
    authorization: Optional[str] = Header(None),
):
    start_time = time.time()
    server_rx_time = start_time
    client_ip = request.client.host if request.client else "unknown"

    # Step 2: Validate token
    verify_bearer_token(authorization)

    # Step 1 & 3: Link Health Tracking & InfluxDB write
    health_state = link_health_tracker.record_packet(
        node_id=payload.node_id,
        seq=payload.seq,
        rx_time=server_rx_time,
    )

    saved = influx_service.write_reading(
        payload=payload,
        server_rx_time=server_rx_time,
        delivery_rate_pct=health_state.delivery_rate_pct,
        gap_seconds=health_state.last_gap_seconds,
    )
    if not saved:
        # Never answer 200 for a reading we did not store: the gateway would treat it as
        # delivered and the reading would vanish. 503 tells the gateway to keep/retry it,
        # and the audit log records why there is a gap.
        logger.error(f"InfluxDB write failed: node={payload.node_id} seq={payload.seq}")
        raise HTTPException(
            status_code=status.HTTP_503_SERVICE_UNAVAILABLE,
            detail="Storage unavailable; reading not saved. Retry later.",
        )

    duration_ms = (time.time() - start_time) * 1000.0

    # Step 4: Structured audit logging
    audit_logger.log_event(
        client_ip=client_ip,
        status_code=status.HTTP_200_OK,
        duration_ms=duration_ms,
        server_rx_time=server_rx_time,
        endpoint="/api/readings",
        payload=payload.model_dump(),
        error=None,
    )

    logger.info(
        f"Ingested node={payload.node_id} seq={payload.seq} type={payload.type} "
        f"temp={payload.temp_c}C verdict={payload.verdict} in {duration_ms:.2f}ms"
    )

    return IngestResponse(
        status="ok",
        node_id=payload.node_id,
        seq=payload.seq,
        verdict=payload.verdict,
        server_rx_time=server_rx_time,
    )


@app.get("/health")
@app.get("/api/health")
async def health_check():
    """Health check endpoint for Docker and monitoring."""
    influx_ok = influx_service.is_healthy()
    uptime = time.time() - START_TIME
    return {
        "status": "healthy" if influx_ok else "degraded",
        "service": "SecureLoRa Ingest",
        "influxdb_connected": influx_ok,
        "uptime_seconds": round(uptime, 1),
    }


@app.get("/api/nodes")
async def get_node_health(authorization: Optional[str] = Header(None)):
    """Inspection endpoint for link health metrics across all nodes."""
    verify_bearer_token(authorization)
    now = time.time()
    states = link_health_tracker.get_all_states()
    return {
        "nodes": [
            {
                "node_id": s.node_id,
                "first_seq": s.first_seq,
                "last_seq": s.last_seq,
                "received_msgs": s.received_msgs,
                "missed_msgs": s.missed_msgs,
                "expected_msgs": s.expected_msgs,
                "delivery_rate_pct": s.delivery_rate_pct,
                "last_gap_seconds": s.last_gap_seconds,
                "current_gap_seconds": s.current_gap_seconds(now),
                "is_stale": s.is_stale(now),
            }
            for s in states
        ]
    }
