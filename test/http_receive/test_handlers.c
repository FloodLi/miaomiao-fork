/* Reuse the fake clock/transport and helper regressions in this translation unit. */
#define main run_helper_tests
#include "test_http_receive.c"
#undef main
#include "host_fs.h"
#include "handler_stubs.h"

/* These macros replace APIs, never the handler control flow. The included file
 * is generated from complete, current production function definitions. */
#define malloc host_malloc
#define free host_free
#include "production_handlers.inc"
#undef malloc
#undef free

static void reset_handler(void)
{
    assert(host_open_files == 0 && live_allocations == 0);
    assert(!ota_active && !display_active && !raw_locked && !layout_locked);
    reset();
    req.content_len = 8;
    host_fail_write = host_fail_close = fail_allocation = false;
    host_write_calls = host_fail_write_call = 0;
    ota_begins = ota_aborts = ota_writes = ota_ends = ota_boots = reboots = 0;
    ota_write_failure = ota_end_failure = false;
    display_begins = display_finishes = raw_locks = layout_takes = layout_gives = 0;
    display_result = ESP_OK;
    conversion_calls = 0;
    compatibility_failure = conversion_failure = false;
    atomic_store(&s_upload_active, false);
    atomic_store(&s_upload_epoch, 0);
    query_name = "same.jpg";
    query_text = NULL;queried_epoch = 0;cached_result = (display_result_t){DISPLAY_RESULT_PENDING,ESP_OK};
    response_status = 200;response_body[0] = 0;host_fail_remove_path = NULL;
    display_superseded = false;epd_ready = true;clear_error = ESP_OK;supersede_on_raw_lock=false;clear_calls=0;
}

enum fault { IDLE, PARTIAL_STALL, EOF_BEFORE_BODY, RECV_ERROR, TRICKLE_TOTAL,
             WRITE_ERROR, CLOSE_ERROR, COPY_ERROR, INVALID_IMAGE, CONVERSION_ERROR,
             BACKUP_CONFLICT, STAGING_CONFLICT, COMPLETE };

static void configure_fault(enum fault fault)
{
    if (fault == IDLE) return;
    event_due = now_us;
    event_result = 8;
    switch (fault) {
    case PARTIAL_STALL: event_result = 2; break;
    case EOF_BEFORE_BODY: event_result = 0; break;
    case RECV_ERROR: event_result = HTTPD_SOCK_ERR_FAIL; break;
    case TRICKLE_TOTAL:
        req.content_len = 1000;
        event_result = 1;
        event_repeat_us = 20000000;
        event_due += event_repeat_us;
        break;
    case WRITE_ERROR: host_fail_write = true; break;
    case CLOSE_ERROR: host_fail_close = true; break;
    case COPY_ERROR: host_fail_write_call = 2; break;
    case INVALID_IMAGE: compatibility_failure = true; break;
    case CONVERSION_ERROR: conversion_failure = true; break;
    default: break;
    }
}

static void assert_released(void)
{
    assert(host_open_files == 0 && live_allocations == 0);
    assert(!ota_active && !display_active && !raw_locked && !layout_locked);
}

static void ota_cleanup(void)
{
    const enum fault faults[] = { IDLE, PARTIAL_STALL, EOF_BEFORE_BODY, RECV_ERROR, TRICKLE_TOTAL };
    for (size_t i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
        reset_handler();
        configure_fault(faults[i]);
        int64_t start = now_us;
        assert(ota_post_handler(&req) == ESP_FAIL);
        assert(ota_begins == 1 && ota_aborts == 1 && ota_ends == 0);
        assert(ota_boots == 0 && reboots == 0 && socket_closed);
        if (faults[i] == PARTIAL_STALL) assert(ota_writes == 1);
        if (faults[i] == TRICKLE_TOTAL) assert(now_us - start == 300000000);
        assert_released();
    }
    reset_handler();
    configure_fault(COMPLETE);
    ota_write_failure = true;
    assert(ota_post_handler(&req) == ESP_FAIL);
    assert(ota_aborts == 1 && ota_writes == 1 && ota_ends == 0 && ota_boots == 0 && reboots == 0);
    assert_released();

    reset_handler();
    fail_allocation = true;
    (void)ota_post_handler(&req);
    assert(ota_aborts == 1 && ota_ends == 0 && ota_boots == 0 && reboots == 0);
    assert_released();

    reset_handler();
    configure_fault(COMPLETE);
    ota_end_failure = true;
    (void)ota_post_handler(&req);
    assert(ota_ends == 1 && ota_aborts == 0 && ota_boots == 0 && reboots == 0);
    assert_released();

    reset_handler();
    configure_fault(COMPLETE);
    assert(ota_post_handler(&req) == ESP_OK);
    assert(ota_writes == 1 && ota_ends == 1 && ota_boots == 1 && reboots == 1 && ota_aborts == 0);
    assert(!socket_closed);
    assert_released();
    puts("OTA handler: deadline/EOF/transport/flash/allocation failures abort; only success selects boot");
}

static void gallery_cleanup(void)
{
    const enum fault faults[] = { IDLE, PARTIAL_STALL, EOF_BEFORE_BODY, RECV_ERROR,
        TRICKLE_TOTAL, WRITE_ERROR, CLOSE_ERROR, COPY_ERROR, INVALID_IMAGE,
        CONVERSION_ERROR, BACKUP_CONFLICT, STAGING_CONFLICT, COMPLETE };
    for (size_t i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
        reset_handler();
        write_file("/spiffs/upload.jpg", "valid upload");
        write_file("/spiffs/images/same.jpg", "valid gallery");
        host_write_calls = 0;
        if (faults[i] == BACKUP_CONFLICT)
            write_file("/spiffs/images/same.jpg.bak", "recovery copy");
        if (faults[i] == STAGING_CONFLICT)
            write_file("/spiffs/images/same.jpg.tmp", "existing neighbor");
        configure_fault(faults[i]);
        (void)gallery_upload_post_handler(&req);
        host_fail_write = host_fail_close = false;
        assert(!gallery_upload_busy() && gallery_upload_epoch() == 1);
        assert(display_begins == 1 && display_finishes == 1);
        assert_released();
        assert(access("/spiffs/http_upload.tmp", F_OK) != 0);
        if (faults[i] == STAGING_CONFLICT) {
            expect_file("/spiffs/images/same.jpg.tmp", "existing neighbor");
            assert(remove("/spiffs/images/same.jpg.tmp") == 0);
        } else assert(access("/spiffs/images/same.jpg.tmp", F_OK) != 0);
        if (faults[i] == COMPLETE) {
            expect_file("/spiffs/images/same.jpg", "xxxxxxxx");
            assert(display_result == ESP_OK && conversion_calls == 1);
        } else {
            expect_file("/spiffs/images/same.jpg", "valid gallery");
            assert(display_result != ESP_OK);
        }
        if (faults[i] <= CLOSE_ERROR) expect_file("/spiffs/upload.jpg", "valid upload");
        if (faults[i] == CONVERSION_ERROR) assert(conversion_calls == 1);
        if (faults[i] == COPY_ERROR || faults[i] == INVALID_IMAGE) assert(conversion_calls == 0);
        if (faults[i] == BACKUP_CONFLICT) {
            expect_file("/spiffs/images/same.jpg.bak", "recovery copy");
            assert(remove("/spiffs/images/same.jpg.bak") == 0);
        } else assert(access("/spiffs/images/same.jpg.bak", F_OK) != 0);
    }
    puts("Gallery handler: existing same-name image survives receive/copy/validation/conversion failures; upload state released");
}

static void canvas_cleanup(void)
{
    const enum fault faults[] = { IDLE, PARTIAL_STALL, EOF_BEFORE_BODY, RECV_ERROR,
        TRICKLE_TOTAL, WRITE_ERROR, CLOSE_ERROR, BACKUP_CONFLICT, COMPLETE };
    for (size_t i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
        reset_handler();
        query_name = "same";
        write_file("/spiffs/cimg/same.bin", "valid canvas");
        if (faults[i] == BACKUP_CONFLICT)
            write_file("/spiffs/cimg/same.bin.bak", "recovery copy");
        configure_fault(faults[i]);
        esp_err_t result = canvas_image_upload_post(&req);
        host_fail_write = host_fail_close = false;
        assert_released();
        assert(access("/spiffs/cimg/same.bin.tmp", F_OK) != 0);
        expect_file("/spiffs/cimg/same.bin", faults[i] == COMPLETE ? "xxxxxxxx" : "valid canvas");
        assert(result == (faults[i] == COMPLETE ? ESP_OK : ESP_FAIL));
        if (faults[i] == BACKUP_CONFLICT) {
            expect_file("/spiffs/cimg/same.bin.bak", "recovery copy");
            assert(remove("/spiffs/cimg/same.bin.bak") == 0);
        } else assert(access("/spiffs/cimg/same.bin.bak", F_OK) != 0);
    }
    for (size_t i = 0; i < 2; i++) {
        reset_handler();
        write_file("/spiffs/canvas_layout.json", "valid layout");
        configure_fault(i == 0 ? PARTIAL_STALL : TRICKLE_TOTAL);
        int64_t start = now_us;
        assert(canvas_layout_post(&req) == ESP_FAIL);
        assert(layout_takes == 1 && layout_gives == 1);
        assert_released();
        expect_file("/spiffs/canvas_layout.json", "valid layout");
        assert(access("/spiffs/canvas_layout.tmp", F_OK) != 0);
        assert(now_us - start == (i == 0 ? 30000000 : 300000000));
    }
    puts("Canvas handlers: existing image preserved, temp files removed, layout mutex released");
}

static void display_queries_and_deletes(void)
{
    char json[192];
    const char *invalid[] = {"display_epoch=", "display_epoch", "display_epoch=0", "display_epoch=-1", "display_epoch=1x", "display_epoch=4294967296", "display_epoch=999999999999999"};
    for (size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
        reset_handler();query_text=invalid[i];
        assert(!http_status_display_result(&req,json,sizeof(json)) && response_status==400 && !queried_epoch);
    }
    reset_handler();assert(http_status_display_result(&req,json,sizeof(json)) && !json[0]);
    query_text="other=value";assert(http_status_display_result(&req,json,sizeof(json)) && !json[0]);
    const char *names[]={"pending","ok","canceled","error"};
    for(int i=0;i<4;++i) {
        reset_handler();query_text="display_epoch=4294967295";cached_result=(display_result_t){i,i==3?ESP_FAIL:ESP_OK};
        assert(http_status_display_result(&req,json,sizeof(json)) && queried_epoch==UINT32_MAX && strstr(json,names[i]));
    }
    cached_result=(display_result_t){DISPLAY_RESULT_ERROR,ESP_ERR_NOT_FOUND};
    assert(http_status_display_result(&req,json,sizeof(json)) && strstr(json,"unavailable"));
    assert(!http_status_display_result(&req,json,8) && response_status==500);
    reset_handler();write_file("/spiffs/images/same.jpg","valid");
    assert(gallery_delete_image_post_handler(&req)==ESP_OK && response_status==200);
    assert(gallery_delete_image_post_handler(&req)==ESP_OK && response_status==404);
    reset_handler();write_file("/spiffs/images/same.jpg","valid");host_fail_remove_path="/spiffs/images/same.jpg";
    assert(gallery_delete_image_post_handler(&req)==ESP_OK && response_status==500);expect_file("/spiffs/images/same.jpg","valid");
    for(int fault=0;fault<6;++fault) {
        reset_handler();req.content_len=0;
        write_file("/spiffs/upload.jpg","valid");write_file("/spiffs/image.bin","valid");
        if(fault==1)clear_error=ESP_FAIL;
        if(fault==2)display_superseded=true;
        if(fault==3)host_fail_remove_path="/spiffs/upload.jpg";
        if(fault==4)epd_ready=false;
        if(fault==5)supersede_on_raw_lock=true;
        assert(gallery_delete_post_handler(&req)==ESP_OK);
        assert(response_status==(fault==0?200:fault==2||fault==5?409:500));
        assert((strstr(response_body,"清屏=完成")!=NULL)==(fault==0||fault==3));
        if(fault==3) { expect_file("/spiffs/upload.jpg","valid");assert(strstr(response_body,"删除失败")); }
        if(fault==5) { expect_file("/spiffs/image.bin","valid");assert(clear_calls==0 && strstr(response_body,"image.bin=未执行")); }
        assert_released();
    }
    reset_handler();req.content_len=0;
    assert(remove("/spiffs/image.bin")==0);
    assert(gallery_delete_post_handler(&req)==ESP_OK && response_status==200 &&
           strstr(response_body,"upload.jpg=不存在") && strstr(response_body,"image.bin=不存在"));
    write_file("/spiffs/upload.jpg","valid upload");write_file("/spiffs/images/same.jpg","valid gallery");
    puts("Display query/deletion handlers: invalid epochs, final results, missing files, partial failures and supersession passed");
}

int main(void)
{
    assert(run_helper_tests() == 0);
    char directory[] = "/tmp/msp-http-handler-data-XXXXXX";
    assert(mkdtemp(directory));
    host_fs_root = directory;
    assert(mkdir("/spiffs/images", 0700) == 0 && mkdir("/spiffs/cimg", 0700) == 0);
    ota_cleanup();
    gallery_cleanup();
    canvas_cleanup();
    display_queries_and_deletes();
    assert_released();
    assert(remove("/spiffs/upload.jpg") == 0);
    assert(remove("/spiffs/images/same.jpg") == 0);
    assert(remove("/spiffs/cimg/same.bin") == 0);
    assert(remove("/spiffs/canvas_layout.json") == 0);
    assert(remove("/spiffs/images") == 0 && remove("/spiffs/cimg") == 0);
    assert(rmdir(directory) == 0);
    puts("HTTP receive production-handler tests: all passed");
    return 0;
}
