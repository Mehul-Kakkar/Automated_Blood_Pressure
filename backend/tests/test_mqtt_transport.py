from types import SimpleNamespace

import pytest

from app.core.config import Settings
from app.messaging.transport import PahoMQTTTransport


class FakePahoClient:
    instances = []

    def __init__(self, **kwargs) -> None:
        self.kwargs = kwargs
        self.instances.append(self)

    def username_pw_set(self, username, password) -> None:
        self.credentials = (username, password)

    def tls_set_context(self, context) -> None:
        self.tls_context = context

    def reconnect_delay_set(self, *, min_delay, max_delay) -> None:
        self.reconnect_delays = (min_delay, max_delay)

    def max_inflight_messages_set(self, count) -> None:
        self.max_inflight = count

    def max_queued_messages_set(self, count) -> None:
        self.max_queued = count

    def connect_async(self, host, port, *, keepalive) -> None:
        self.connection = (host, port, keepalive)


def settings(**kwargs) -> Settings:
    return Settings(
        _env_file=None,
        mqtt_enabled=True,
        mqtt_host="k7128701.ala.asia-southeast1.emqxsl.com",
        mqtt_port=8883,
        mqtt_username="bp_backend",
        mqtt_password="test-only-secret",
        database_url="postgresql+psycopg://test:test@localhost/test",
        **kwargs,
    )


def test_paho_transport_keeps_tls_verification_and_uses_unique_instance_ids(monkeypatch) -> None:
    from paho.mqtt import client as mqtt

    FakePahoClient.instances.clear()
    monkeypatch.setattr(mqtt, "Client", FakePahoClient)

    first = PahoMQTTTransport(settings())
    second = PahoMQTTTransport(settings())
    first.connect_async()

    first_client, second_client = FakePahoClient.instances
    assert first_client.kwargs["client_id"] != second_client.kwargs["client_id"]
    assert first_client.kwargs["protocol"] == mqtt.MQTTv5
    assert first_client.kwargs["manual_ack"] is True
    assert first_client.credentials == ("bp_backend", "test-only-secret")
    assert first_client.tls_context.check_hostname is True
    assert first_client.tls_context.verify_mode == 2  # ssl.CERT_REQUIRED
    assert first_client.reconnect_delays == (1, 60)
    assert first_client.connection == ("k7128701.ala.asia-southeast1.emqxsl.com", 8883, 60)


def test_paho_transport_rejects_missing_ca_file(monkeypatch, tmp_path) -> None:
    from paho.mqtt import client as mqtt

    monkeypatch.setattr(mqtt, "Client", FakePahoClient)
    with pytest.raises(ValueError, match="CA file does not exist"):
        PahoMQTTTransport(settings(mqtt_ca_file=str(tmp_path / "missing-ca.pem")))
