"""paho-mqtt wrapper bridged into asyncio.

paho-mqtt's threaded loop (loop_start) is the most robust mode for a
long-lived service. We let it run on its own thread and bridge inbound
state messages back into the asyncio event loop via
`loop.call_soon_threadsafe` + an `asyncio.Queue`. Publish is fire-and-
forget from the asyncio side (paho's queue is thread-safe).

What this module owns:
  - One paho.Client.
  - The "last known device state" cache (read from retained
    jarvis/state on (re)connect, updated on every message).
  - An asyncio.Queue of state-change events that the router task drains
    to trigger drains of the medium queue.

What it deliberately doesn't own:
  - The medium-queue itself or any drain policy. State messages just
    surface here; the router decides what to do.
"""

from __future__ import annotations

import asyncio
import logging
from typing import Awaitable, Callable

import paho.mqtt.client as mqtt

from . import config

log = logging.getLogger(__name__)

# Async callback shape: `async def on_state(prev: str | None, new: str) -> None`.
StateCallback = Callable[[str | None, str], Awaitable[None]]


class MqttBridge:
    def __init__(self, loop: asyncio.AbstractEventLoop) -> None:
        self._loop = loop
        self._state: str | None = None
        self._connected = False
        # Newest-state-only queue: drain semantics only ever care about
        # the latest known state, but we want the consumer task to react
        # to *every* IDLE transition, so we use a real queue (not a
        # cell) and let the consumer dedup.
        self._state_events: asyncio.Queue[tuple[str | None, str]] = asyncio.Queue()
        self._client = mqtt.Client(
            client_id=config.MQTT_CLIENT_ID,
            callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
        )
        self._client.on_connect = self._on_connect
        self._client.on_disconnect = self._on_disconnect
        self._client.on_message = self._on_message
        if config.MQTT_USER:
            self._client.username_pw_set(config.MQTT_USER, config.MQTT_PASS)
        # paho's reconnect backoff: starts at 1s, doubles to 120s. We
        # let paho own reconnect cadence — nothing custom on top.
        self._client.reconnect_delay_set(min_delay=1, max_delay=120)

    # ── Lifecycle ────────────────────────────────────────────────────
    def start(self) -> None:
        log.info(
            "mqtt connecting host=%s port=%d client_id=%s user=%s",
            config.MQTT_HOST, config.MQTT_PORT, config.MQTT_CLIENT_ID,
            config.MQTT_USER or "<anon>",
        )
        try:
            # connect_async returns immediately; loop_start spawns the
            # network thread that handles the actual connect + reconnect.
            self._client.connect_async(
                config.MQTT_HOST, config.MQTT_PORT, config.MQTT_KEEPALIVE_SEC
            )
        except Exception as exc:
            # Bad host etc. Log and keep loop_start so it keeps retrying.
            log.error("mqtt connect_async failed: %s", exc)
        self._client.loop_start()

    def stop(self) -> None:
        log.info("mqtt stopping")
        try:
            self._client.disconnect()
        finally:
            self._client.loop_stop()

    # ── paho callbacks (background thread) ───────────────────────────
    def _on_connect(self, client, _userdata, _flags, reason_code, _properties=None):  # noqa: ANN001
        if reason_code == 0:
            self._connected = True
            log.info("mqtt connected; subscribing to %s", config.MQTT_TOPIC_STATE)
            # qos=1 so we don't miss the retained state on a flaky link.
            client.subscribe(config.MQTT_TOPIC_STATE, qos=1)
        else:
            log.error("mqtt connect failed: rc=%s", reason_code)

    def _on_disconnect(self, _client, _userdata, _flags, reason_code, _properties=None):  # noqa: ANN001
        self._connected = False
        log.warning("mqtt disconnected: rc=%s (paho will reconnect)", reason_code)

    def _on_message(self, _client, _userdata, msg):  # noqa: ANN001
        if msg.topic != config.MQTT_TOPIC_STATE:
            return
        try:
            payload = msg.payload.decode("utf-8", errors="replace").strip()
        except Exception:
            return
        prev = self._state
        self._state = payload
        log.info("mqtt state %s -> %s", prev, payload)
        # Bridge to asyncio. call_soon_threadsafe is the only async-safe
        # entry point from a non-loop thread.
        try:
            self._loop.call_soon_threadsafe(
                self._state_events.put_nowait, (prev, payload)
            )
        except RuntimeError:
            # Loop closed during shutdown — drop the event silently.
            pass

    # ── Public surface ───────────────────────────────────────────────
    @property
    def connected(self) -> bool:
        return self._connected

    @property
    def last_state(self) -> str | None:
        return self._state

    async def next_state_event(self) -> tuple[str | None, str]:
        """Await the next (prev, new) state event from the device.
        Returns when an event arrives — no timeout. The caller is
        responsible for shielding / cancellation."""
        return await self._state_events.get()

    def publish_speak(self, text: str) -> bool:
        """Publish a TTS string to jarvis/speak. Returns True if paho
        accepted it for delivery (which doesn't guarantee the broker or
        the device received it — QoS=1, broker side handles retransmit)."""
        if not self._connected:
            # Pre-connect publishes are silently queued by paho but we
            # surface the warning so callers can decide whether to retry.
            log.warning(
                "publish_speak while disconnected; paho will buffer (qos=1)"
            )
        info = self._client.publish(
            config.MQTT_TOPIC_SPEAK,
            payload=text.encode("utf-8"),
            qos=1,
            retain=False,
        )
        return info.rc == mqtt.MQTT_ERR_SUCCESS
