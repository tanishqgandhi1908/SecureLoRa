import time
from dataclasses import dataclass
from typing import Optional
from app.logger import logger
from app.config import settings


@dataclass
class NodeHealthState:
    node_id: int
    first_seen_time: float
    last_rx_time: float
    first_seq: int
    last_seq: int
    received_msgs: int = 0
    missed_msgs: int = 0
    last_gap_seconds: float = 0.0

    @property
    def expected_msgs(self) -> int:
        return self.received_msgs + self.missed_msgs

    @property
    def delivery_rate_pct(self) -> float:
        if self.expected_msgs == 0:
            return 100.0
        return round((self.received_msgs / self.expected_msgs) * 100.0, 2)

    def is_stale(self, now: Optional[float] = None) -> bool:
        current_time = now or time.time()
        return (current_time - self.last_rx_time) > settings.stale_threshold_seconds

    def current_gap_seconds(self, now: Optional[float] = None) -> float:
        current_time = now or time.time()
        return round(current_time - self.last_rx_time, 2)


class LinkHealthTracker:
    """
    In-memory state tracker for node sequence counters, packet delivery rate,
    gaps, and staleness detection.
    """

    def __init__(self):
        self.nodes: dict[int, NodeHealthState] = {}

    def record_packet(self, node_id: int, seq: int, rx_time: float) -> NodeHealthState:
        if node_id not in self.nodes:
            state = NodeHealthState(
                node_id=node_id,
                first_seen_time=rx_time,
                last_rx_time=rx_time,
                first_seq=seq,
                last_seq=seq,
                received_msgs=1,
                missed_msgs=0,
                last_gap_seconds=0.0,
            )
            self.nodes[node_id] = state
            logger.info(f"LinkHealth: Node {node_id} registered at seq {seq}")
            return state

        state = self.nodes[node_id]
        time_diff = rx_time - state.last_rx_time
        state.last_gap_seconds = round(time_diff, 2)

        if seq > state.last_seq + 1:
            missed = seq - state.last_seq - 1
            state.missed_msgs += missed
            logger.warning(
                f"LinkHealth: Node {node_id} missed {missed} packets "
                f"(SEQ jumped from {state.last_seq} to {seq}, gap: {state.last_gap_seconds}s)"
            )
        elif seq <= state.last_seq:
            logger.info(
                f"LinkHealth: Node {node_id} SEQ {seq} <= last {state.last_seq} "
                f"(node reboot or retransmission)"
            )

        state.received_msgs += 1
        state.last_seq = seq
        state.last_rx_time = rx_time

        logger.info(
            f"LinkHealth: Node {node_id} SEQ={seq} | "
            f"rx={state.received_msgs} missed={state.missed_msgs} "
            f"rate={state.delivery_rate_pct}% gap={state.last_gap_seconds}s"
        )
        return state

    def get_stale_nodes(self, now: Optional[float] = None) -> list[NodeHealthState]:
        current_time = now or time.time()
        return [state for state in self.nodes.values() if state.is_stale(current_time)]

    def get_all_states(self) -> list[NodeHealthState]:
        return list(self.nodes.values())


link_health_tracker = LinkHealthTracker()
