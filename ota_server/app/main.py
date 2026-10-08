import json
import logging
import tempfile
from contextlib import asynccontextmanager
from pathlib import Path
from urllib.parse import urlencode

from fastapi import Depends, FastAPI, File, Form, Header, HTTPException, Request, UploadFile, status
from fastapi.exceptions import RequestValidationError
from fastapi.responses import HTMLResponse, JSONResponse, RedirectResponse
from fastapi.staticfiles import StaticFiles
from fastapi.templating import Jinja2Templates

from .admin import build_dashboard, build_release_rows, build_report_sessions
from .config import ServerConfig, load_config
from .publisher import publish_release_archive
from .schemas import ApiResponse, CheckRequest, CheckResponse, RegisterRequest, ReportRequest
from .storage import OTAStorage


logger = logging.getLogger("ota_server")
APP_DIR = Path(__file__).resolve().parent
templates = Jinja2Templates(directory=str(APP_DIR / "templates"))


def setup_logging() -> None:
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s %(message)s",
    )


def create_app(config_path: str | None = None) -> FastAPI:
    config = load_config(config_path)
    storage = OTAStorage(config)

    @asynccontextmanager
    async def lifespan(_: FastAPI):
        setup_logging()
        storage.init_db()
        logger.info(
            "ota server config loaded host=%s port=%s db=%s packages=%s logs=%s proxy_required=%s",
            config.server_host,
            config.server_port,
            config.database_path,
            config.package_root,
            config.log_dir,
            config.require_https_proxy,
        )
        yield

    app = FastAPI(title="OTA Server", lifespan=lifespan)
    app.state.config = config
    app.state.storage = storage
    app.mount("/static", StaticFiles(directory=str(APP_DIR / "static")), name="static")
    app.mount(config.package_url_prefix, StaticFiles(directory=str(config.package_root)), name="packages")

    @app.exception_handler(HTTPException)
    async def http_exception_handler(_: Request, exc: HTTPException) -> JSONResponse:
        detail = exc.detail if isinstance(exc.detail, str) else "request failed"
        return JSONResponse(
            status_code=exc.status_code,
            content={"code": exc.status_code, "message": detail},
        )

    @app.exception_handler(RequestValidationError)
    async def validation_exception_handler(_: Request, exc: RequestValidationError) -> JSONResponse:
        message = "; ".join(error["msg"] for error in exc.errors())
        return JSONResponse(
            status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
            content={"code": status.HTTP_422_UNPROCESSABLE_ENTITY, "message": message},
        )

    def get_config() -> ServerConfig:
        return app.state.config

    def get_storage() -> OTAStorage:
        return app.state.storage

    def render_page(
        request: Request,
        template_name: str,
        *,
        title: str,
        nav: str,
        subtitle: str = "",
        message: str = "",
        message_type: str = "",
        **context,
    ) -> HTMLResponse:
        payload = {
            "request": request,
            "title": title,
            "subtitle": subtitle,
            "nav": nav,
            "message": message,
            "message_type": message_type,
            "public_base_url": config.public_base_url,
        }
        payload.update(context)
        return templates.TemplateResponse(name=template_name, context=payload, request=request)

    def require_token(
        request: Request,
        config: ServerConfig = Depends(get_config),
        x_ota_token: str | None = Header(default=None),
    ) -> None:
        if x_ota_token != config.api_token:
            raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail="invalid api token")
        if config.require_https_proxy:
            header_value = request.headers.get(config.https_proxy_header)
            if header_value != "https":
                raise HTTPException(
                    status_code=status.HTTP_400_BAD_REQUEST,
                    detail=f"missing https proxy header: {config.https_proxy_header}=https",
                )

    @app.get("/api/v1/healthz", response_model=ApiResponse)
    async def healthz() -> ApiResponse:
        return ApiResponse()

    @app.get("/", include_in_schema=False)
    async def root() -> RedirectResponse:
        return RedirectResponse(url="/admin", status_code=status.HTTP_303_SEE_OTHER)

    @app.get("/admin", response_class=HTMLResponse, include_in_schema=False)
    async def admin_dashboard(
        request: Request,
        store: OTAStorage = Depends(get_storage),
    ) -> HTMLResponse:
        return render_page(
            request,
            "dashboard.html",
            title="概览",
            subtitle="这里能看到设备、发布包和最近升级结果。",
            nav="dashboard",
            dashboard=build_dashboard(store, config),
        )

    @app.get("/admin/devices", response_class=HTMLResponse, include_in_schema=False)
    async def admin_devices(
        request: Request,
        store: OTAStorage = Depends(get_storage),
    ) -> HTMLResponse:
        return render_page(
            request,
            "devices.html",
            title="设备列表",
            subtitle="设备通过 register 接口登记后，会在这里出现。",
            nav="devices",
            devices=[dict(row) for row in store.list_devices(limit=200)],
        )

    @app.get("/admin/releases", response_class=HTMLResponse, include_in_schema=False)
    async def admin_releases(
        request: Request,
        status_text: str = "",
        message: str = "",
        store: OTAStorage = Depends(get_storage),
    ) -> HTMLResponse:
        return render_page(
            request,
            "releases.html",
            title="发布管理",
            subtitle="通过网页上传 OTA 压缩包并生成稳定下载地址。",
            nav="releases",
            releases=build_release_rows(store, config),
            message=message,
            message_type=status_text,
        )

    @app.post("/admin/releases/publish", include_in_schema=False)
    async def admin_publish_release(
        release_id: str = Form(...),
        board_type: str = Form(...),
        release_note: str = Form(default=""),
        mandatory: str | None = Form(default=None),
        package_file: UploadFile = File(...),
        store: OTAStorage = Depends(get_storage),
    ) -> RedirectResponse:
        suffix = Path(package_file.filename or "").suffix.lower()
        if not (package_file.filename or "").endswith((".tar.gz", ".tgz")):
            return RedirectResponse(
                url="/admin/releases?" + urlencode(
                    {"status_text": "error", "message": "只支持上传 .tar.gz 或 .tgz 文件"}
                ),
                status_code=status.HTTP_303_SEE_OTHER,
            )

        with tempfile.NamedTemporaryFile(delete=False, suffix=suffix or ".tar.gz") as temp_file:
            temp_path = Path(temp_file.name)
            temp_file.write(await package_file.read())

        try:
            publish_release_archive(
                config=config,
                storage=store,
                archive_path=temp_path,
                release_id=release_id.strip(),
                board_type=board_type.strip(),
                mandatory=mandatory == "1",
                release_note=release_note.strip(),
            )
        except ValueError as exc:
            temp_path.unlink(missing_ok=True)
            await package_file.close()
            return RedirectResponse(
                url="/admin/releases?" + urlencode({"status_text": "error", "message": str(exc)}),
                status_code=status.HTTP_303_SEE_OTHER,
            )

        temp_path.unlink(missing_ok=True)
        await package_file.close()
        return RedirectResponse(
            url="/admin/releases?" + urlencode({"status_text": "success", "message": f"发布成功：{release_id}"}),
            status_code=status.HTTP_303_SEE_OTHER,
        )

    @app.post("/admin/releases/{release_id}/delete", include_in_schema=False)
    async def admin_delete_release(
        release_id: str,
        store: OTAStorage = Depends(get_storage),
    ) -> RedirectResponse:
        release = store.delete_release(release_id.strip(), delete_files=True)
        if release is None:
            return RedirectResponse(
                url="/admin/releases?" + urlencode(
                    {"status_text": "error", "message": f"发布不存在：{release_id}"}
                ),
                status_code=status.HTTP_303_SEE_OTHER,
            )
        manifest_path = config.package_root.parent / "manifests" / f"{release['board_type']}.json"
        if manifest_path.is_file():
            try:
                manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            except json.JSONDecodeError:
                manifest = {}
            if manifest.get("release_id") == release_id:
                manifest_path.unlink()
        return RedirectResponse(
            url="/admin/releases?" + urlencode(
                {"status_text": "success", "message": f"已删除发布：{release_id}"}
            ),
            status_code=status.HTTP_303_SEE_OTHER,
        )

    @app.get("/admin/reports", response_class=HTMLResponse, include_in_schema=False)
    async def admin_reports(
        request: Request,
        release_id: str = "",
        device_id: str = "",
        result: str = "",
        store: OTAStorage = Depends(get_storage),
    ) -> HTMLResponse:
        reports = [
            dict(row) for row in store.list_reports_filtered(
                release_id=release_id.strip(),
                device_id=device_id.strip(),
                result=result.strip(),
                limit=500,
            )
        ]
        return render_page(
            request,
            "reports.html",
            title="升级记录",
            subtitle="按一次升级会话聚合，便于查看每台设备每个 release 的阶段推进。",
            nav="reports",
            reports=reports,
            report_sessions=build_report_sessions(reports),
            filter_options=store.list_report_filter_options(),
            filters={"release_id": release_id, "device_id": device_id, "result": result},
        )

    @app.post("/api/v1/devices/register", response_model=ApiResponse, dependencies=[Depends(require_token)])
    async def register_device(
        payload: RegisterRequest,
        store: OTAStorage = Depends(get_storage),
    ) -> ApiResponse:
        store.upsert_device(payload)
        return ApiResponse()

    @app.post("/api/v1/ota/check", response_model=CheckResponse, dependencies=[Depends(require_token)])
    async def check_update(
        payload: CheckRequest,
        store: OTAStorage = Depends(get_storage),
    ) -> CheckResponse:
        release = store.find_update(payload)
        if release is None:
            return CheckResponse(has_update=False, release=None)
        return CheckResponse(has_update=True, release=release)

    @app.post("/api/v1/ota/report", response_model=ApiResponse, dependencies=[Depends(require_token)])
    async def report_result(
        payload: ReportRequest,
        store: OTAStorage = Depends(get_storage),
    ) -> ApiResponse:
        try:
            store.insert_report(payload)
        except LookupError as exc:
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail=str(exc)) from exc
        except ValueError as exc:
            raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail=str(exc)) from exc
        return ApiResponse()

    return app


app = create_app()


if __name__ == "__main__":
    import uvicorn

    cfg = load_config()
    uvicorn.run("ota_server.app.main:app", host=cfg.server_host, port=cfg.server_port, reload=False)
