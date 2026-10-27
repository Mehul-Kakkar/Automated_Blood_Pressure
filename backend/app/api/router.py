from fastapi import APIRouter

from app.api.routes.health import router as health_router
from app.api.routes.sessions import router as sessions_router
from app.api.routes.reports import router as reports_router
from app.api.routes.devices import router as devices_router
from app.api.routes.profile import router as profile_router

api_router = APIRouter()
api_router.include_router(health_router, tags=["health"])
api_router.include_router(sessions_router)
api_router.include_router(reports_router)
api_router.include_router(devices_router)
api_router.include_router(profile_router)
