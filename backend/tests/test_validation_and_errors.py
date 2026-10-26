from fastapi.testclient import TestClient


def test_invalid_query_input_uses_consistent_error_envelope(client: TestClient) -> None:
    response = client.get("/ready?check_database=perhaps")

    assert response.status_code == 422
    body = response.json()
    assert body["error"]["code"] == "request_validation_error"
    assert body["error"]["message"] == "Request validation failed."
    assert body["error"]["request_id"] == response.headers["x-request-id"]
    assert body["error"]["details"][0]["location"] == "query.check_database"
    assert "input" not in str(body).lower()


def test_not_found_uses_consistent_error_envelope(client: TestClient) -> None:
    response = client.get("/missing")

    assert response.status_code == 404
    assert response.json()["error"]["code"] == "not_found"


def test_untrusted_request_id_is_replaced(client: TestClient) -> None:
    response = client.get("/health", headers={"X-Request-ID": "bad id\r\n"})

    assert response.status_code == 200
    assert response.headers["x-request-id"] != "bad id\r\n"
