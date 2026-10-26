from fastapi.testclient import TestClient


def test_health_reports_liveness_without_database(client: TestClient) -> None:
    response = client.get("/health")

    assert response.status_code == 200
    assert response.json() == {"status": "ok", "service": "Experimental Device API"}
    assert response.headers["x-request-id"]


def test_readiness_without_database_is_explicit(client: TestClient) -> None:
    response = client.get("/ready")

    assert response.status_code == 200
    assert response.json() == {"status": "ready", "database": {"status": "not_configured"}}


def test_readiness_checks_database_when_configured(
    client: TestClient, monkeypatch
) -> None:
    from app.api.routes import health as health_routes
    from app.core.config import get_settings

    monkeypatch.setenv("DATABASE_URL", "postgresql+psycopg://placeholder:placeholder@localhost/db")
    get_settings.cache_clear()
    client.app.state.settings = get_settings()
    monkeypatch.setattr(health_routes, "probe_database_connection", lambda _settings: True)

    response = client.get("/ready")

    assert response.status_code == 200
    assert response.json()["database"]["status"] == "available"


def test_readiness_returns_service_unavailable_on_database_failure(
    client: TestClient, monkeypatch
) -> None:
    from app.api.routes import health as health_routes
    from app.core.config import get_settings

    monkeypatch.setenv("DATABASE_URL", "postgresql+psycopg://placeholder:placeholder@localhost/db")
    get_settings.cache_clear()
    client.app.state.settings = get_settings()
    monkeypatch.setattr(health_routes, "probe_database_connection", lambda _settings: False)

    response = client.get("/ready")

    assert response.status_code == 503
    assert response.json()["database"]["status"] == "unavailable"


def test_cors_allows_only_configured_origin(client: TestClient) -> None:
    allowed = client.options(
        "/health",
        headers={"Origin": "http://localhost:5173", "Access-Control-Request-Method": "GET"},
    )
    denied = client.options(
        "/health",
        headers={"Origin": "https://unlisted.example", "Access-Control-Request-Method": "GET"},
    )

    assert allowed.headers.get("access-control-allow-origin") == "http://localhost:5173"
    assert "access-control-allow-origin" not in denied.headers
