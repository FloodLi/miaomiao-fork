#!/usr/bin/env python3
"""Check public repository documentation against current project files.

This script is intentionally read-only. It verifies the facts that are easy to
let drift in README/release documents before publishing to Gitee.
"""

from __future__ import annotations

import hashlib
import os
import re
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

EXPECTED = {
    "project_version": "2.3.6",
    "http_routes": 81,
    "web_html": 12,
    "web_png": 1,
    "web_js": 2,
    "release_bin_sha256": "37D164F2B06F87AF1BBBAF3E6A8273FD389A69C23CD2D07222E4AA4100048628",
    "release_zip_sha256": "55CA82559ADD81EEFDE965E6AADA2D1918CA8DD52558E2AF6FF14BA39C60723C",
    "web_upgrade_bin_sha256": "3624AB9EE2E9BCFF37170B8B8CE158EFFB193CDEC5A942F08F2520215A001A18",
    "web_upgrade_zip_sha256": "49CC67E85F12F58838D5A919FEF4BD77DF38C32B27B58036B61776E1894E79AC",
}

EXPECTED_PARTITIONS = {
    "nvs": ("0x9000", "0xC000"),
    "otadata": ("0x15000", "0x2000"),
    "phy_init": ("0x17000", "0x1000"),
    "factory": ("0x20000", "0x300000"),
    "ota_0": ("0x320000", "0x300000"),
    "ota_1": ("0x620000", "0x300000"),
    "coredump": ("0x920000", "0x20000"),
    "fontfs": ("0x940000", "0x2E0000"),
    "spiffs": ("0xC20000", "0x3E0000"),
}

PUBLIC_TEXT_EXTENSIONS = {
    ".c", ".h", ".js", ".cjs", ".sh", ".toml",
    ".csv",
    ".html",
    ".json",
    ".md",
    ".ps1",
    ".py",
    ".txt",
    ".yml",
    ".yaml",
}

FORBIDDEN_SCAN_EXCLUDES = {
    "tools/check_public_docs.py",
}

FORBIDDEN_PATTERNS = [
    r"本次会话",
    r"working tree",
    r"COM82",
    r"monitor 进程",
    r"AppData",
    r"wxid_",
    r"Tencent Files",
    r"C:\\Users",
    r"D:/wx",
    r"v2\.3\.1",
    r"v2\.3\.2",
    r"v2\.3\.8",
    r"墨鱼",
    r"墨水鱼",
    r"Moy",
    r"sk-[A-Za-z0-9]",
    r"AIza",
    r"hf_[A-Za-z0-9]",
    r"BEGIN (RSA|OPENSSH|PRIVATE) KEY",
]


def run_git(*args: str) -> str:
    return subprocess.check_output(
        ["git", "-c", "core.quotePath=false", *args],
        cwd=ROOT,
        text=True,
        encoding="utf-8",
        errors="replace",
    )


def read_text(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8", errors="replace")


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with (ROOT / path).open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest().upper()


def check(condition: bool, label: str, failures: list[str], detail: str = "") -> None:
    if condition:
        print(f"OK  {label}")
    else:
        suffix = f": {detail}" if detail else ""
        print(f"ERR {label}{suffix}")
        failures.append(f"{label}{suffix}")


def check_project_version(failures: list[str]) -> None:
    cmake = read_text("CMakeLists.txt")
    match = re.search(r'set\(PROJECT_VER\s+"([^"]+)"\)', cmake)
    version = match.group(1) if match else None
    check(version == EXPECTED["project_version"], f"PROJECT_VER is {EXPECTED['project_version']}", failures, str(version))


def check_web_embed(failures: list[str]) -> None:
    cmake = read_text("main/CMakeLists.txt")
    block = re.search(r"EMBED_FILES(.*?)PRIV_REQUIRES", cmake, re.S)
    embedded = re.findall(r"\$\{PROJECT_DIR\}/web/([^\"\s]+)", block.group(1) if block else "")
    html = [p for p in embedded if p.endswith(".html")]
    png = [p for p in embedded if p.endswith(".png")]
    js = [p for p in embedded if p.endswith(".js")]
    check(len(html) == EXPECTED["web_html"], "embedded HTML count is 12", failures, str(html))
    check(len(png) == EXPECTED["web_png"], "embedded PNG count is 1", failures, str(png))
    check(len(js) == EXPECTED["web_js"], "embedded JavaScript count is 2", failures, str(js))
    for name in embedded:
        check((ROOT / "web" / name).exists(), f"embedded web resource exists: {name}", failures)


def check_web_feature_help(failures: list[str]) -> None:
    missing: list[str] = []
    invalid: list[str] = []
    for path in sorted((ROOT / "web").glob("*.html")):
        text = path.read_text(encoding="utf-8", errors="replace")
        if 'src="feature_help.js"' not in text or "featureHelpPopover" not in text or "info-dot" not in text:
            missing.append(path.name)
            continue
        buttons = re.findall(r'<button\b[^>]*class="[^"]*info-dot[^"]*"[^>]*>', text)
        if not buttons or any("aria-label=" not in button or "data-help=" not in button for button in buttons):
            invalid.append(path.name)
    check(not missing, "all Web pages expose contextual help", failures, ", ".join(missing))
    check(not invalid, "Web help buttons include labels and content", failures, ", ".join(invalid))


def check_web_collapsible_settings(failures: list[str]) -> None:
    shared = read_text("web/feature_help.js")
    check(all(value in shared for value in (
        ".more-settings", "content:'展开'", "content:'收起'",
        "window.epdExpandMoreSettings",
    )), "shared more-settings component exists", failures)

    expected = {
        "index.html": ('id="advanced"',),
        "gallery.html": ("slideshowMoreSettings",),
        "weather.html": ("weatherMoreSettings",),
        "codex.html": ("codexMoreSettings",),
        "timetable.html": ("timetableMoreSettings", "timetablePeriodsSettings"),
        "config.html": ("panelMoreSettings", "welcomeMoreSettings",
                        "powerMoreSettings", "buttonModesMoreSettings"),
        "message.html": ("msg-advanced more-settings",),
    }
    missing: list[str] = []
    opened: list[str] = []
    for name, markers in expected.items():
        text = read_text(f"web/{name}")
        for marker in markers:
            if marker not in text:
                missing.append(f"{name}:{marker}")
        if re.search(r'<details\b[^>]*\bopen(?:\s|=|>)', text):
            opened.append(name)
    check(not missing, "long Web pages expose more-settings sections", failures,
          ", ".join(missing))
    check(not opened, "more-settings sections default to collapsed", failures,
          ", ".join(opened))
    check("openWeatherMore()" in read_text("web/weather.html") and
          "openTimetableSettings" in read_text("web/timetable.html") and
          "buttonModesMoreSettings" in read_text("web/config.html"),
          "hidden validation fields expand on demand", failures)


def check_http_routes(failures: list[str]) -> None:
    text = read_text("main/http_app.c")
    try:
        start = text.index("const httpd_uri_t uris[] = {")
        end = text.index("    };", start)
    except ValueError:
        check(False, "HTTP route table found", failures)
        return
    body = text[start:end]
    entries = re.findall(r'\{\s*"([^"]+)"\s*,\s*(HTTP_[A-Z]+)\s*,', body)
    check(len(entries) == EXPECTED["http_routes"], "HTTP route count is 81", failures, str(len(entries)))


def check_panel_config_reboot(failures: list[str]) -> None:
    http = read_text("main/http_app.c")
    web = read_text("web/config.html")
    check(all(value in http for value in (
        "reboot_scheduled",
        '"panel_reboot"',
        "if (reboot_required)",
    )), "panel configuration schedules automatic reboot", failures)
    check(all(value in web for value in (
        "j.reboot_scheduled",
        "设备将在约 2 秒后自动重启",
        "保存成功后设备会自动重启",
        "自动重启任务启动失败",
    )), "panel configuration UI handles automatic reboot", failures)


def check_offline_calendar_time(failures: list[str]) -> None:
    time_sync = read_text("main/time_sync.c")
    app = read_text("main/app_main.c")
    check(all(value in time_sync for value in (
        "time_sync_restore_offline_time",
        "TIME_NVS_LAST_EPOCH",
        "firmware_build_epoch",
        "schedule_trusted_time_save",
        "TIME_SAVE_MIN_INTERVAL_S",
    )), "offline calendar has trusted time fallback", failures)
    check("time_sync_restore_offline_time()" in app and
          app.index("time_sync_restore_offline_time()") <
          app.index("if (power_mgr_is_timer_wake())") and
          "esp_err_t time_err = time_sync_init()" in app,
          "offline time restores before boot routing and SNTP starts in AP mode",
          failures)


def check_startup_display_transactions(failures: list[str]) -> None:
    policy = read_text("main/display_policy.c")
    mode = read_text("main/display_mode.c")
    app = read_text("main/app_main.c")
    scheduler = read_text("main/scheduler.c")
    power = read_text("main/power_mgr.c")
    welcome = read_text("main/welcome_config.c")
    calendar = read_text("main/calendar_display.c")
    http = read_text("main/http_app.c")
    web = read_text("web/config.html")
    cmake = read_text("main/CMakeLists.txt")

    check(all(value in policy for value in (
        "display_policy_begin_request",
        "display_policy_finish_request",
        "token.previous_manual = s_committed_manual",
        '"stale"',
        "portENTER_CRITICAL(&s_policy_mux)",
    )), "display owner state commits through an epoch transaction", failures)
    check("display_policy_finish_request(&token, err, final_manual)" in mode and
          "if (!m->available)" in mode and "m->wait_complete" in mode,
          "display modes commit atomically and honor availability", failures)
    check("welcome_config_get(&welcome_cfg)" in app and
          "if (!welcome_cfg.enabled)" in app and
          "scheduler_restore_last_image" in app,
          "global welcome policy and exact gallery restore are wired", failures)
    check("NVS_KEY_LAST_IMAGE \"last_img\"" in scheduler and
          "BIT_BOOT_COMPLETE" in scheduler and "portMAX_DELAY" in scheduler,
          "slideshow persists last image and quick boot cannot time out", failures)
    check('"service_status.c"' in cmake and
          (ROOT / "main/service_status.c").exists(),
          "optional-service status registry is built", failures)
    check((ROOT / "test/startup/main/test_startup_state.c").exists(),
          "startup transaction host tests exist", failures)
    check('"welcome_config.c"' in cmake and
          http.count('{ "/welcome_config"') == 2 and
          all(value in welcome for value in (
              'NVS_NS = "welcome"', '"title"', '"message"',
              'out->enabled = true')) and
          all(value in web for value in (
              'id="welcomeTitle"', 'id="welcomeMessage"',
              'loadWelcomeConfig()', "welcome:'/welcome_config'")) and
          all(value in app for value in (
              "welcome_config_get(&welcome_cfg)",
              "if (!welcome_cfg.enabled)",
              "render_welcome_screen(&welcome_cfg, welcome.epoch)")),
          "welcome page config is persisted, served, and backed up", failures)
    check("DEFAULT_DISPLAY_MODE   DISPLAY_MODE_CALENDAR" in power and
          "waiting_for_time" in calendar and "等待时间同步" in calendar,
          "fresh NVS defaults to calendar mode", failures)


def check_jd79668_semantic_ui(failures: list[str]) -> None:
    theme = read_text("main/ui_color_theme.c")
    weather = read_text("main/weather.c")
    http = read_text("main/http_app.c")
    web = read_text("web/config.html")
    epd = read_text("main/epd_stub.c")

    check("epd_get_panel() == EPD_PANEL_REF_JD79668_42_BWRY" in theme,
          "semantic UI is restricted to JD79668", failures)
    check(all(name in theme for name in ("balanced", "warm", "vivid", "mono")),
          "all four semantic color profiles exist", failures)
    check(http.count('{ "/ui_color_config"') == 2,
          "UI color GET/POST routes exist", failures)
    selectors = re.findall(r'<select\s+data-ui-color-page="[^"]+"', web)
    check(len(selectors) == 9,
          "nine page color selectors exist", failures)
    check("ui_color:'/ui_color_config'" in web,
          "UI color config participates in backup restore", failures)
    check(all(rule in weather for rule in (
        "code == 100 || code == 150",
        "code >= 101 && code <= 103",
        "code >= 151 && code <= 153",
        "code >= 302 && code <= 304",
        "code >= 503 && code <= 508",
        "code == 900",
    )), "weather semantic code rules exist", failures)
    check("JD79668  BWRY  TEST" in epd and
          all(label in epd for label in ('"BLACK"', '"WHITE"', '"RED"', '"YELLOW"')),
          "JD79668 four-color test pattern exists", failures)


def check_jd79665_image_profile(failures: list[str]) -> None:
    image = read_text("main/image_convert.c")
    epd = read_text("main/epd_stub.c")
    web = read_text("web/index.html") + read_text("web/image_processor.js")

    check("case EPD_PANEL_REF_JD79665_75_BWRY: return &s_quant_jd79665_75" in image and
          "case EPD_PANEL_REF_JD79668_42_BWRY: return &s_quant_jd79668_42" in image,
          "JD image profiles select exact IDs 34 and 37", failures)
    check(all(value in image for value in (
        ".bw_luma_threshold = 104",
        ".fs_error_gain_q8 = 192",
        ".red_gain_q8 = 280",
        ".yellow_gain_q8 = 288",
    )), "JD79665 firmware image profile values exist", failures)
    check("epd_panel_image_profile(panel, &image_profile)" in read_text("main/http_app.c") and
          '"jd79665-v1"' in epd and '"jd79668-v1"' in epd and '"e6-v1"' in epd,
          "status image profiles derive from driver descriptors and exact IDs", failures)
    check("function profileFromStatus" in web and "s.panel!==43" in web and
          "JSON.stringify(p.inks)!==JSON.stringify(types)" in web,
          "browser validates capabilities without resolution-based E6 detection", failures)
    check("function jd75InkCoverage" in web and "input.inks.includes('red')" in web and
          "input.inks.includes('yellow')" in web and "function ditherPreviewE6SixColor" in web,
          "coverage respects allowed inks and retains separate E6 core", failures)
    check("function profileDefaults" in web and "profile.id===34" in web and
          "![34,37].includes(profile.id)" in web,
          "panel-specific tuning does not leak to other four-color panels", failures)
    check('id="imageWorkspaceTemplate"' in web and "attachShadow({mode:'open'})" in web and
          'id="previewDialog"' not in web and "PRESET_VALUES" not in web,
          "isolated workspace replaces old nested preview and presets", failures)
    check("function average" in web and "this.compare?this.original:this.frame" in web,
          "display resampling and original comparison remain separate from upload", failures)
    check("upload_editor_v4_panel_" in web and "new Worker('/image_processor.js')" in web and
          "ready!==this.revision" in web and "profileId!==this.profile?.profileId" in web,
          "multi-panel workspace gates jobs and PNG upload by identity", failures)
    check("'upload_img_opts_panel_'+profile.id" in web and "'upload_editor_v3_panel_'+profile.id" in web,
          "legacy settings migrate without overwriting old keys", failures)
    check("png_try_prequantized_passthrough" in image and
          image.index("png_try_prequantized_passthrough") < image.index("image_quant_log_profile(\"PNG\""),
          "prequantized PNG bypasses the JD79665 fallback quantizer", failures)


def check_visionpeak_ei0583_panel(failures: list[str]) -> None:
    header = read_text("main/epd.h")
    epd = read_text("main/epd_stub.c")
    http = read_text("main/http_app.c")
    web = read_text("web/config.html")
    readme = read_text("README.md")

    check("EPD_PANEL_VISIONPEAK_EI0583T5PCZ_BW = 30" in header and
          "EPD_PANEL_REF_UC8159_583_BW      = EPD_PANEL_VISIONPEAK_EI0583T5PCZ_BW" in header,
          "Vision Peak EI0583T5PCZ keeps panel ID 30 and its legacy alias", failures)
    descriptor = next((line for line in epd.splitlines()
                       if "[EPD_PANEL_VISIONPEAK_EI0583T5PCZ_BW]" in line), "")
    check(all(value in descriptor for value in (
        "{ 600, 448, false, 1, false",
        "EPD_CMD_DATA_UC8159_INTERLEAVED",
        "ref_uc8159_init",
        "ref_uc81xx_refresh",
        ".optional_refresh_caps = 0",
    )), "Vision Peak EI0583T5PCZ descriptor is 600x448 BW full-refresh only", failures,
          descriptor.strip())
    check("epd_panel_name(panel)" in http and
          '<option value="30">5.83 寸 | 黑白 | 视峰 EI0583T5PCZ / UC8159 兼容 | 600×448 | 实验（仅全刷）' in web and
          "selected===29||selected===30||selected===48" in web,
          "Vision Peak EI0583T5PCZ is selectable with BUSY idle-high default", failures)
    check("Vision Peak EI0583T5PCZ hardware uses BUSY idle-high; migrating busy_idle -> 1" not in epd and
          "if (panel_is_visionpeak_ei0583_bw())\n        return 1;" not in epd and
          "pending_panel == EPD_PANEL_VISIONPEAK_EI0583T5PCZ_BW" not in http and
          "if(panel===30){busy_idle=1" not in web,
          "Vision Peak EI0583T5PCZ preserves explicit BUSY overrides", failures)
    check("视峰 EI0583T5PCZ / WF0583T5PCZ 5.83\" BW" in readme and
          "参考驱动推断" in readme and "待实测确认" in readme and
          "4bit/像素" in readme and
          "目前仅开放全刷" in readme,
          "Vision Peak EI0583T5PCZ assumptions and wire format are documented", failures)


def check_uc8159_low_bw_panel(failures: list[str]) -> None:
    header = read_text("main/epd.h")
    epd = read_text("main/epd_stub.c")
    fb = read_text("main/fb_render.c")
    web = read_text("web/config.html")
    readme = read_text("README.md")

    check("EPD_PANEL_REF_UC8159_75_LOW_BW   = 46" in header,
          "UC8159 low-resolution BW panel keeps ID 46", failures)
    descriptor = next((line for line in epd.splitlines()
                       if "[EPD_PANEL_REF_UC8159_75_LOW_BW]" in line), "")
    check(all(value in descriptor for value in (
        "{ 640, 384, false, 0, false",
        "EPD_CMD_DATA_UC8159_INTERLEAVED",
        ".optional_refresh_caps = 0",
    )), "UC8159 640x384 BW descriptor is full-refresh only", failures,
          descriptor.strip())
    check('<option value="46">7.5 寸 | 黑白 | UC8159 低分辨率 | 640×384' in web,
          "UC8159 low-resolution BW panel is selectable", failures)
    check("UC8159 7.5\" 低分辨率 BW" in readme and "日常刷新仅开放全刷" in readme,
          "UC8159 low-resolution BW limits are documented", failures)
    check("epd_uc8159_refresh_test" in epd and
          all(value in web for value in (
              "testExperimentalRefresh('fast')",
              "testExperimentalRefresh('partial')",
              "/epd_test?mode=",
              "#experimentalRefreshTestPanel[hidden]{display:none!important}",
          )), "UC8159 isolated refresh tests are wired to the config page", failures)
    check("UC8159 7.5 low-resolution BW hardware uses BUSY idle-low" in epd and
          "UC8159 refresh experiment requires tested BUSY idle-low" in epd and
          "if(selected===8||selected===46||selected===47)document.getElementById('pnBusy').value='0'" in web,
          "UC8159 hardware restores the tested BUSY idle-low setting", failures)
    check((ROOT / "test/epd/test_epd.c").exists(),
          "EPD BUSY failure regression test source exists", failures)
    check("if (epd_has_red() && fb->red)" in fb and
          "if (fb->red)\n            fb->red[byte_idx] &= ~mask;" in fb,
          "BW-only render views do not dereference a missing red plane", failures)
    check("uc8159_bw_render_regression_check" in epd and
          'ui_draw_fixed_text(&fb, 4, 8, "WHITE", COLOR_WHITE, 1)' in epd,
          "UC8159 test runs the BW-only white-text render regression check", failures)


def check_uc8179_75_bwr_panel(failures: list[str]) -> None:
    header = read_text("main/epd.h")
    epd = read_text("main/epd_stub.c")
    web = read_text("web/config.html")
    readme = read_text("README.md")

    check("EPD_PANEL_REF_UC8179_75_BWR      = 29" in header,
          "GDEY075Z08 UC8179 panel keeps ID 29", failures)
    descriptor = next((line for line in epd.splitlines()
                       if "[EPD_PANEL_REF_UC8179_75_BWR]" in line), "")
    check(all(value in descriptor for value in (
        "{ 800, 480, true,  1, true",
        "EPD_CMD_DATA_UC8179_75_BWR",
        "ref_uc8179_75_bwr_init",
        "ref_uc8179_75_bwr_refresh",
        ".optional_refresh_caps = EPD_REFRESH_CAP_PARTIAL_COLOR",
    )), "GDEY075Z08 descriptor enables guarded BW partial refresh", failures,
          descriptor.strip())
    check(all(value in epd for value in (
        "cmd4(0x01, 0x07, 0x07, 0x3F, 0x3F)",
        "cmd4(0x06, 0x17, 0x17, 0x28, 0x17)",
        "cmd2(0x50, 0x13, 0x07)",
        "EPD_UC8179_75_BUSY_UPDATE_TIMEOUT_MS  35000",
        "GDEY075Z08 UC8179 uses BUSY idle-high",
    )), "GDEY075Z08 uses its dedicated UC8179 full-refresh setup", failures)
    check('<option value="29">7.5 寸 | 黑白红 | UC8179 / GDEY075Z08 | 800×480 高清 | 实验（黑白真局刷）' in web and
          "else if(selected===29||selected===30||selected===48)document.getElementById('pnBusy').value='1'" in web,
          "GDEY075Z08 is selectable with BUSY idle-high", failures)
    check('GDEY075Z08 7.5" 高清 BWR' in readme and
          "UC8159 对应 640×384 低分辨率型号" in readme and
          "红色变化或窗口覆盖红色时自动全刷" in readme and
          "每 8 次局刷维护全刷" in readme,
          "GDEY075Z08 controller and limits are documented", failures)
    check(all(value in epd for value in (
        "GDEY075Z08 UC8179 BW partial LUT loaded (0x20..0x25)",
        "uc8179_75_write_bw_region(0x13, old_data, rect)",
        "uc8179_75_write_bw_region(0x10, new_data, rect)",
        "s_uc8179_red_signature",
        "red-plane-changed",
        "partial-window-overlaps-red",
        "UC8179_75_PARTIAL_REFRESH_LIMIT     8",
    )), "GDEY075Z08 BW partial path preserves OLD/NEW and color safety", failures)


def check_p426010_panel(failures: list[str]) -> None:
    header = read_text("main/epd.h")
    epd = read_text("main/epd_stub.c")
    web = read_text("web/config.html")
    readme = read_text("README.md")

    check("EPD_PANEL_WS_426_BW     = 8" in header,
          "P426010 4.26 BW panel keeps historical ID 8", failures)
    descriptor = next((line for line in epd.splitlines()
                       if "[EPD_PANEL_WS_426_BW]" in line), "")
    check(all(value in descriptor for value in (
        "{ 800, 480, false, 0, false",
        "EPD_CMD_DATA_24_26",
        "ws_426_bw_init",
        "refresh_22_f7_20",
        ".optional_refresh_caps = 0",
    )), "P426010 descriptor is 800x480 BW full-refresh only", failures,
          descriptor.strip())
    check(all(value in epd for value in (
        "cmd5(0x0C, 0xAE, 0xC7, 0xC3, 0xC0, 0x80)",
        "cmd3(0x01, 0xDF, 0x01, 0x02)",
        "cmd4(0x44, 0x00, 0x00, 0x1F, 0x03)",
        "cmd4(0x45, 0xDF, 0x01, 0x00, 0x00)",
        "P426010 4.26 BW uses BUSY idle-low",
    )), "P426010 uses the official EPD_4in26 full-refresh setup", failures)
    check('<option value="8">4.26 寸 | 黑白 | P426010-MF1-B / 微雪 | 800×480' in web and
          "if(selected===8||selected===46||selected===47)" in web,
          "P426010 is selectable with BUSY idle-low Web default", failures)
    check("P426010-MF1-B / 微雪 4.26\" BW" in readme and
          "日常仅开放全刷" in readme,
          "P426010 limits and adapter requirement are documented", failures)


def check_ssd16xx_213_panel(failures: list[str]) -> None:
    header = read_text("main/epd.h")
    epd = read_text("main/epd_stub.c")
    http = read_text("main/http_app.c")
    web = read_text("web/config.html")
    readme = read_text("README.md")

    check("EPD_PANEL_SSD16XX_213_BW         = 47" in header,
          "SSD16xx 2.13 128x250 panel keeps ID 47", failures)
    descriptor = next((line for line in epd.splitlines()
                       if "[EPD_PANEL_SSD16XX_213_BW]" in line), "")
    check(all(value in descriptor for value in (
        "SSD16XX_213_RAM_W, SSD16XX_213_H, false, 0, false",
        "EPD_CMD_DATA_24_26",
        "ssd16xx_213_bw_init",
        "ssd16xx_213_bw_refresh",
        "SSD16XX_213_X_OFFSET",
        ".optional_refresh_caps = 0",
    )), "SSD16xx 2.13 descriptor is 128x250 RAM BW full-refresh only", failures,
          descriptor.strip())
    check(all(value in epd for value in (
        "#define SSD16XX_213_ACTIVE_W  122",
        "#define SSD16XX_213_H         250",
        "return panel_is_hink_29() || panel_is_ssd16xx_213_bw()",
        "cmd1(0x22, candidate == EPD_REFRESH_FAST ? 0xC7 : 0xFF)",
        "result->w = candidate == EPD_REFRESH_PARTIAL ? 64 : SSD16XX_213_RAM_W",
        "SSD16xx 2.13 128x250 refresh experiment start",
    )), "SSD16xx 2.13 geometry and isolated refresh candidates exist", failures)
    check("epd_ssd16xx_213_refresh_test(candidate, &result)" in http and
          '<option value="47">2.13 寸 | 黑白 | SSD16xx | 128×250' in web and
          "pnActivePanel!==46&&pnActivePanel!==47" in web,
          "SSD16xx 2.13 tests are restricted to panel ID 47", failures)
    check("SSD16xx 2.13\" BW" in readme and
          "有效区 122×250" in readme,
          "SSD16xx 2.13 active and RAM geometry are documented", failures)


def check_gd7965_uc8179_75_bw_panel(failures: list[str]) -> None:
    header = read_text("main/epd.h")
    epd = read_text("main/epd_stub.c")
    http = read_text("main/http_app.c")
    web = read_text("web/config.html")
    readme = read_text("README.md")

    check("EPD_PANEL_GD7965_UC8179_75_BW    = 48" in header,
          "GD7965/UC8179 7.5 BW panel keeps ID 48", failures)
    descriptor = next((line for line in epd.splitlines()
                       if "[EPD_PANEL_GD7965_UC8179_75_BW]" in line), "")
    check(all(value in descriptor for value in (
        "{ 800, 480, false, 1, true",
        "EPD_CMD_DATA_UC81XX_REF",
        "gd7965_uc8179_75_bw_init",
        ".optional_refresh_caps = 0",
    )), "GD7965/UC8179 800x480 descriptor is full-refresh only", failures,
          descriptor.strip())
    check(all(value in epd for value in (
        "cmd4(0x01, 0x07, 0x07, 0x3F, 0x3F)",
        "cmd2(0x50, 0x29, 0x07)",
        "cmd1(0x00, 0x3F)",
        "cmd1(0x82, 0x26)",
        "cmd2(0x50, 0x39, 0x07)",
        "{ 0x5A, 30, 5, 30, 5, 1 }",
        "{ 0x84, 30, 5, 30, 5, 1 }",
        "epd_gd7965_75_refresh_test",
        "result->w = candidate == EPD_REFRESH_PARTIAL ? 400 : epd_width()",
    )), "GD7965/UC8179 isolated GDEW075T7 refresh candidate exists", failures)
    check("epd_gd7965_75_refresh_test(candidate, &result)" in http and
          '<option value="48">GD7965 / UC8179 | 7.5 寸 | 黑白 | 800×480 | 实验（仅全刷）' in web and
          "pnActivePanel!==46&&pnActivePanel!==47&&pnActivePanel!==48" in web and
          "selected===29||selected===30||selected===48" in web,
          "GD7965/UC8179 tests are restricted to panel ID 48 with BUSY idle-high", failures)
    check('GD7965 / UC8179 7.5" BW' in readme and
          "`HT075A04` 仅作为屏体识别备注" in readme and
          "实屏通过前不会用于时钟、图片或自动刷新" in readme,
          "GD7965/UC8179 identification and refresh limits are documented", failures)


def check_partitions(failures: list[str]) -> None:
    rows = {}
    for raw in read_text("partitions.csv").splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        cols = [c.strip() for c in line.rstrip(",").split(",")]
        if len(cols) >= 5:
            rows[cols[0]] = (cols[3], cols[4])
    for name, expected in EXPECTED_PARTITIONS.items():
        check(rows.get(name) == expected, f"partition {name} is {expected[0]} {expected[1]}", failures, str(rows.get(name)))


def check_release_hashes(failures: list[str]) -> None:
    bin_hash = sha256_file("release/epaper_uploader_full_16MB.bin")
    zip_hash = sha256_file("release/epaper_uploader_full_16MB.zip")
    web_bin_hash = sha256_file("release/web_upgrade.bin")
    web_zip_hash = sha256_file("release/web_upgrade.zip")
    check(bin_hash == EXPECTED["release_bin_sha256"], "release bin SHA256 matches docs", failures, bin_hash)
    check(zip_hash == EXPECTED["release_zip_sha256"], "release zip SHA256 matches docs", failures, zip_hash)
    check(web_bin_hash == EXPECTED["web_upgrade_bin_sha256"], "web upgrade bin SHA256 matches docs", failures, web_bin_hash)
    check(web_zip_hash == EXPECTED["web_upgrade_zip_sha256"], "web upgrade zip SHA256 matches docs", failures, web_zip_hash)


def check_markdown_links(failures: list[str]) -> None:
    tracked = tracked_files()
    missing: list[str] = []
    for path in sorted(p for p in tracked if p.lower().endswith(".md")):
        text = read_text(path)
        for match in re.finditer(r"!{0,1}\[[^\]]*\]\(([^)]+)\)", text):
            target = match.group(1).strip()
            if target.startswith("<") and target.endswith(">"):
                target = target[1:-1]
            target = target.split("#", 1)[0].strip()
            if not target or re.match(r"^[a-zA-Z][a-zA-Z0-9+.-]*:", target):
                continue
            norm = os.path.normpath(os.path.join(os.path.dirname(path), target.replace("\\", "/"))).replace("\\", "/")
            if norm.startswith("../"):
                continue
            if norm not in tracked and not any(x.startswith(norm.rstrip("/") + "/") for x in tracked):
                line = text.count("\n", 0, match.start()) + 1
                missing.append(f"{path}:{line} -> {target}")
    check(not missing, "Markdown local links resolve", failures, "; ".join(missing[:10]))


def check_forbidden_text(failures: list[str]) -> None:
    hits: list[str] = []
    for path in sorted(public_text_files()):
        if path in FORBIDDEN_SCAN_EXCLUDES:
            continue
        text = read_text(path)
        for pattern in FORBIDDEN_PATTERNS:
            regex = re.compile(pattern)
            for match in regex.finditer(text):
                line = text.count("\n", 0, match.start()) + 1
                hits.append(f"{path}:{line}:{pattern}")
    check(not hits, "public docs have no banned local/private markers", failures, "; ".join(hits[:10]))


def tracked_files() -> set[str]:
    # Include new public files; ignored local handoffs, environments and outputs stay out.
    files = {p.replace("\\", "/") for p in run_git("ls-files", "--cached", "--others", "--exclude-standard").splitlines()}
    return {p for p in files if (ROOT / p).exists()}


def public_text_files() -> list[str]:
    files: list[str] = []
    for path in tracked_files():
        suffix = Path(path).suffix.lower()
        if suffix in PUBLIC_TEXT_EXTENSIONS:
            files.append(path)
    return files


def main() -> int:
    failures: list[str] = []
    check_project_version(failures)
    check_web_embed(failures)
    check_web_feature_help(failures)
    check_web_collapsible_settings(failures)
    check_http_routes(failures)
    check_panel_config_reboot(failures)
    check_offline_calendar_time(failures)
    check_startup_display_transactions(failures)
    check_jd79668_semantic_ui(failures)
    check_jd79665_image_profile(failures)
    check_p426010_panel(failures)
    check_ssd16xx_213_panel(failures)
    check_gd7965_uc8179_75_bw_panel(failures)
    check_visionpeak_ei0583_panel(failures)
    check_uc8159_low_bw_panel(failures)
    check_uc8179_75_bwr_panel(failures)
    check_partitions(failures)
    check_release_hashes(failures)
    check_markdown_links(failures)
    check_forbidden_text(failures)

    if failures:
        print("\nFAILED")
        for item in failures:
            print(f"- {item}")
        return 1

    print("\nPublic documentation checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
