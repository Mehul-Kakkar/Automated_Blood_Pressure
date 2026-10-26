"""Paho MQTT 5 adapter with strict TLS verification and bounded client queues."""

import ssl
from pathlib import Path
from typing import Any, Callable, Protocol

from paho.mqtt import client as mqtt
from paho.mqtt.packettypes import PacketTypes
from paho.mqtt.properties import Properties

from app.core.config import Settings


class IncomingMessage(Protocol):
    topic: str
    payload: bytes
    qos: int
    mid: int


class MQTTTransport(Protocol):
    on_connect: Callable[..., None] | None
    on_disconnect: Callable[..., None] | None
    on_message: Callable[..., None] | None

    def connect_async(self) -> None: ...
    def loop_start(self) -> None: ...
    def loop_stop(self) -> None: ...
    def disconnect(self) -> None: ...
    def subscribe(self, subscriptions: list[tuple[str, int]]) -> Any: ...
    def publish(self, topic: str, payload: bytes, *, expiry_seconds: int, timeout_seconds: int) -> None: ...
    def ack(self, mid: int, qos: int) -> None: ...
    def is_connected(self) -> bool: ...


class PahoMQTTTransport:
    def __init__(self, settings: Settings) -> None:
        if not settings.mqtt_configured:
            raise ValueError("MQTT broker and backend service credentials are not configured.")
        if settings.mqtt_ca_file and not Path(settings.mqtt_ca_file).is_file():
            raise ValueError("Configured MQTT CA file does not exist.")
        if settings.mqtt_client_cert_file and not Path(settings.mqtt_client_cert_file).is_file():
            raise ValueError("Configured MQTT client certificate file does not exist.")
        if settings.mqtt_client_key_file and not Path(settings.mqtt_client_key_file).is_file():
            raise ValueError("Configured MQTT client key file does not exist.")

        tls_context = ssl.create_default_context(cafile=settings.mqtt_ca_file)
        tls_context.minimum_version = ssl.TLSVersion.TLSv1_2
        tls_context.verify_mode = ssl.CERT_REQUIRED
        tls_context.check_hostname = True
        if settings.mqtt_client_cert_file and settings.mqtt_client_key_file:
            tls_context.load_cert_chain(settings.mqtt_client_cert_file, settings.mqtt_client_key_file)

        self._settings = settings
        self.on_connect: Callable[..., None] | None = None
        self.on_disconnect: Callable[..., None] | None = None
        self.on_message: Callable[..., None] | None = None
        self._client = mqtt.Client(
            callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
            client_id=settings.mqtt_client_id,
            protocol=mqtt.MQTTv5,
            reconnect_on_failure=True,
            manual_ack=True,
        )
        assert settings.mqtt_username is not None and settings.mqtt_password is not None
        self._client.username_pw_set(settings.mqtt_username, settings.mqtt_password.get_secret_value())
        self._client.tls_set_context(tls_context)
        self._client.max_inflight_messages_set(20)
        self._client.max_queued_messages_set(settings.mqtt_queue_size)
        self._client.on_connect = self._on_connect
        self._client.on_disconnect = self._on_disconnect
        self._client.on_message = self._on_message

    def _on_connect(self, client: Any, userdata: Any, flags: Any, reason_code: Any, properties: Any) -> None:
        if self.on_connect:
            self.on_connect(client, userdata, flags, reason_code, properties)

    def _on_disconnect(self, client: Any, userdata: Any, disconnect_flags: Any, reason_code: Any, properties: Any) -> None:
        if self.on_disconnect:
            self.on_disconnect(client, userdata, disconnect_flags, reason_code, properties)

    def _on_message(self, client: Any, userdata: Any, message: IncomingMessage) -> None:
        if self.on_message:
            self.on_message(client, userdata, message)

    def connect_async(self) -> None:
        assert self._settings.mqtt_host is not None
        self._client.connect_async(self._settings.mqtt_host, self._settings.mqtt_port, keepalive=30)

    def loop_start(self) -> None:
        self._client.loop_start()

    def loop_stop(self) -> None:
        self._client.loop_stop()

    def disconnect(self) -> None:
        self._client.disconnect()

    def subscribe(self, subscriptions: list[tuple[str, int]]) -> Any:
        return self._client.subscribe(subscriptions)

    def publish(self, topic: str, payload: bytes, *, expiry_seconds: int, timeout_seconds: int) -> None:
        if not self._client.is_connected():
            raise ConnectionError("MQTT broker is disconnected; command was not queued for later replay.")
        properties = Properties(PacketTypes.PUBLISH)
        properties.MessageExpiryInterval = max(1, expiry_seconds)
        info = self._client.publish(topic, payload, qos=1, retain=False, properties=properties)
        if info.rc != mqtt.MQTT_ERR_SUCCESS:
            raise ConnectionError("MQTT broker rejected the command publish request.")
        info.wait_for_publish(timeout=timeout_seconds)
        if not info.is_published():
            raise TimeoutError("MQTT broker did not acknowledge the command before timeout.")

    def ack(self, mid: int, qos: int) -> None:
        if qos > 0:
            self._client.ack(mid, qos)

    def is_connected(self) -> bool:
        return self._client.is_connected()
