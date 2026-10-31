"""Host checks of production C code with mocked RTOS/HTTP/LVGL APIs; no hardware.

Run: python tools/test_power_efficiency.py (requires GCC on PATH).
Generated harnesses and executables live under build/power-efficiency-tests/.
"""
from datetime import datetime, timezone
from pathlib import Path
import re
import json
import os
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/power-efficiency-tests'
COMMON = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <ctype.h>
#include <inttypes.h>
#include <time.h>
#include <setjmp.h>
#ifdef _WIN32
static struct tm *host_gmtime_r(const time_t *epoch,struct tm *out) {return gmtime_s(out,epoch)==0?out:NULL;}
#define gmtime_r host_gmtime_r
#endif
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_INVALID_SIZE 3
#define ESP_ERR_NO_MEM 4
#define ESP_ERR_TIMEOUT 5
#define ESP_ERR_HTTP_CONNECT 6
#define ESP_ERR_HTTP_CONNECTION_CLOSED 7
#define ESP_ERR_HTTP_WRITE_DATA 8
#define ESP_ERR_HTTP_FETCH_HEADER 9
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_ERROR_CHECK(x) assert((x) == ESP_OK)
#define configASSERT(x) assert(x)
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
typedef void *SemaphoreHandle_t;
static bool mutex_available = true;
static int mutex_held;
static SemaphoreHandle_t xSemaphoreCreateMutex(void) {return (void *)1;}
static int xSemaphoreTake(SemaphoreHandle_t sem, int timeout) {if (!mutex_available) return 0; assert(!mutex_held); mutex_held=1; return 1;}
static void xSemaphoreGive(SemaphoreHandle_t sem) {assert(mutex_held); mutex_held=0;}
'''

def source(path):
    return (ROOT / path).read_text(encoding='utf-8')


def function(text, name):
    """Extract current production function, never a separately maintained copy."""
    match = re.search(r'^[\w *]+\b' + re.escape(name) + r'\([^;]*?\)\s*\{', text, re.M)
    if not match:
        raise AssertionError(f'Function not found: {name}')
    start = match.start()
    brace = text.index('{', match.start())
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end] + '\n'


def run(name, text, extra=()):
    BUILD.mkdir(parents=True, exist_ok=True)
    c = BUILD / (name + '.c')
    exe = BUILD / (name + '.exe')
    c.write_text(COMMON + text, encoding='utf-8')
    subprocess.run([shutil.which('gcc') or 'gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-function', '-Wno-unused-variable', '-Wno-unused-parameter',
                    str(c), *map(str,extra), '-o', str(exe)], check=True, timeout=30)
    subprocess.run([str(exe)], check=True, timeout=30)


def epoch(value):
    return int(datetime.fromisoformat(value).replace(tzinfo=timezone.utc).timestamp())


def schedule_tests():
    ny = function(source('components/time_manager/time_manager.c'), 'time_manager_get_new_york_time')
    ny += function(source('components/stock_monitor/src/stock_monitor.c'), 'stock_monitor_is_market_open')
    net = re.sub(r'^#include.*\n', '', source('main/network_sync.c'), flags=re.M)
    mock = r'''
#define NETWORK_SYNC_STOCKS 1U
#define NETWORK_SYNC_WEATHER 2U
typedef int portMUX_TYPE;
typedef void *TaskHandle_t;
typedef int BaseType_t;
typedef uint32_t TickType_t;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
#define WIFI_POWER_SYNC 1
static time_t wall;
static int64_t monotonic;
static bool valid_clock=true, connected=true;
static int stock_calls, weather_calls, time_calls, holds;
static int stock_error, weather_error, ntp_error;
static int connect_seconds, fetch_seconds;
static time_t ntp_epoch;
static jmp_buf sleeping;
static time_t fake_time(time_t *out) {if(out)*out=wall; return wall;}
#define time fake_time
static int64_t esp_timer_get_time(void) {return monotonic;}
static bool time_manager_has_valid_time(void) {return valid_clock;}
static esp_err_t time_manager_sync(int timeout) {time_calls++; if(ntp_epoch) {wall=ntp_epoch;valid_clock=true;} return ntp_error;}
static esp_err_t wifi_power_acquire(int owner) {holds++;return ESP_OK;}
static void wifi_power_release(int owner) {assert(holds==1);holds--;}
static bool wifi_wait_connected(int timeout) {wall+=connect_seconds;monotonic+=(int64_t)connect_seconds*1000000;return connected;}
static esp_err_t stock_monitor_refresh(void) {stock_calls++;wall+=fetch_seconds;monotonic+=(int64_t)fetch_seconds*1000000;return stock_error;}
static esp_err_t weather_refresh(void) {weather_calls++;wall+=fetch_seconds;monotonic+=(int64_t)fetch_seconds*1000000;return weather_error;}
static void xTaskNotifyGive(void *task) {}
static void xTaskNotifyWait(int a,uint32_t b,uint32_t *out,TickType_t wait) {assert(wait>0 && holds==0);longjmp(sleeping,1);}
static int xTaskCreatePinnedToCore(void (*fn)(void *),const char *name,int stack,void *arg,int priority,void **handle,int core) {return pdPASS;}
'''
    cases = []
    # Fixed official transition dates, including leap years; independent expected offsets.
    for year, march, november in [(2024,10,3),(2025,9,2),(2026,8,1),(2027,14,7),(2028,12,5),(2029,11,4),(2030,10,3)]:
        for utc, hour, minute, second, dst in [
            (f'{year}-03-{march:02}T06:59:59',1,59,59,0),
            (f'{year}-03-{march:02}T07:00:00',3,0,0,1),
            (f'{year}-11-{november:02}T05:59:59',1,59,59,1),
            (f'{year}-11-{november:02}T06:00:00',1,0,0,0)]:
            cases.append(f'assert(time_manager_get_new_york_time({epoch(utc)}LL,&ny)); assert(ny.tm_hour=={hour} && ny.tm_min=={minute} && ny.tm_sec=={second} && ny.tm_isdst=={dst});')
    for utc, expected in [('2026-01-12T14:30:00',1),('2026-07-13T13:30:00',1),('2026-03-16T13:30:00',1),('2026-10-26T13:30:00',1),('2026-01-12T14:25:00',0),('2026-01-12T21:00:00',1),('2026-01-12T21:05:00',0),('2026-01-17T14:30:00',0),('2026-01-18T14:30:00',0),('2026-07-03T13:30:00',1)]:
        cases.append(f'assert(is_stock_slot({epoch(utc)}LL)=={expected});')
    for utc, expected in [('2026-01-12T14:29:59',0),('2026-01-12T14:30:00',1),('2026-01-12T14:31:00',1),('2026-01-12T20:59:59',1),('2026-01-12T21:00:00',0),('2026-01-12T21:00:01',0),('2026-07-13T13:30:00',1),('2026-03-16T13:30:00',1),('2026-10-26T13:30:00',1),('2026-01-17T14:30:00',0),('2026-01-18T14:30:00',0),('2026-07-03T13:30:00',1)]:
        cases.append(f'assert(stock_monitor_is_market_open({epoch(utc)}LL)=={expected});')
    cases.append('assert(!stock_monitor_is_market_open(0));')
    for now, nxt in [('2026-01-12T14:01:00','2026-01-12T14:30:00'),('2026-01-12T14:31:00','2026-01-12T14:35:00'),('2026-01-12T20:56:00','2026-01-12T21:00:00'),('2026-01-12T21:01:00','2026-01-12T22:00:00'),('2026-01-17T14:31:00','2026-01-17T15:00:00')]:
        cases.append(f'assert(next_slot_epoch({epoch(now)}LL)=={epoch(nxt)}LL);')
    scenarios = r'''
static void reset_window(time_t epoch, bool startup, bool due) {
 wall=epoch;monotonic=1000000;valid_clock=connected=true;
 stock_calls=weather_calls=time_calls=holds=0;
 stock_error=weather_error=ntp_error=connect_seconds=fetch_seconds=0;ntp_epoch=0;
 s_pending_work=s_retry_work=0;s_retry_time=false;
 s_startup_sync_pending=startup;s_task=NULL;s_last_hour_queued=epoch/3600;
 s_next_slot_epoch=due?epoch:next_slot_epoch(epoch);
 s_next_slot_us=due?monotonic:monotonic+(s_next_slot_epoch-epoch)*1000000;
}
static void window(void) {if(setjmp(sleeping)==0)network_sync_task(NULL);assert(holds==0);}
'''
    main = r'''
int main(void) {
 struct tm ny;
 assert(!time_manager_get_new_york_time(0,NULL));
'''+ '\n'.join(cases) +r'''
 // Boot and manual stock work bypass the weekend gate.
 reset_window(1768658400,true,false);window();assert(stock_calls==1 && weather_calls==1 && time_calls==1);
 reset_window(1768658400,false,false);network_sync_request(NETWORK_SYNC_STOCKS);time_t target=s_next_slot_epoch;window();assert(stock_calls==1 && !weather_calls && s_next_slot_epoch==target);
 // Closed-market hourly retry must retain stocks without fetching them.
 reset_window(1768255200,false,true);s_last_hour_queued--;s_retry_work=NETWORK_SYNC_STOCKS;window();assert(!stock_calls && weather_calls==1 && time_calls==1 && s_retry_work==NETWORK_SYNC_STOCKS);
 reset_window(1768228200,false,true);s_retry_work=NETWORK_SYNC_STOCKS;window();assert(stock_calls==1 && !s_retry_work);
 // 16:00 coalesces all three jobs; normal delay still includes the close quote.
 reset_window(1768251600,false,true);s_last_hour_queued--;connect_seconds=20;window();assert(stock_calls==1 && weather_calls==1 && time_calls==1);
 // A failed quote becomes pending; the deadline still advances.
 reset_window(1768228200,false,true);stock_error=ESP_FAIL;window();assert(s_retry_work==NETWORK_SYNC_STOCKS && s_next_slot_epoch==1768228500);
 // A manual connection crosses the opening boundary: one quote batch, no duplicate window.
 reset_window(1768228190,false,false);network_sync_request(NETWORK_SYNC_WEATHER);connect_seconds=20;window();assert(stock_calls==1 && weather_calls==1 && s_next_slot_epoch==1768228500);
 // Manual NTP plus a slow weather fetch must not postpone the opening slot.
 reset_window(1768228170,false,false);s_retry_time=true;fetch_seconds=40;network_sync_request(NETWORK_SYNC_WEATHER);window();assert(stock_calls==1 && weather_calls==1);
 // Invalid clock: no HTTPS, retry after five minutes. NTP recovery realigns to an hour off market.
 reset_window(0,true,true);valid_clock=false;ntp_error=ESP_ERR_TIMEOUT;window();assert(!stock_calls && !weather_calls && s_retry_time && s_retry_work==3 && s_next_slot_us==monotonic+300000000);
 reset_window(0,true,true);valid_clock=false;ntp_epoch=1768658400;window();assert(stock_calls==1 && weather_calls==1 && time_calls==1 && s_next_slot_epoch==next_slot_epoch(wall));
 // NTP jumps forward across market close: no automatic stale quote.
 reset_window(1768251300,false,true);s_retry_time=true;ntp_epoch=1768255500;window();assert(!stock_calls && s_next_slot_epoch==next_slot_epoch(wall));
 puts("PASS: New York DST, market boundaries and network task scenarios");
}
'''
    run('schedule',ny+mock+net+scenarios+main)


def display_tests():
    text = source('components/display/src/display.c')
    definitions = '\n'.join(re.findall(r'^#define DISPLAY_(?:IDLE_PERIOD_MS|ACTIVE_PERIOD_MS).*',text,re.M))
    mock = r'''
#define CONFIG_PM_ENABLE 1
typedef struct {uint32_t period;int ready;} lv_timer_t;
static lv_timer_t refresh, input, animation;
static lv_timer_t *s_refresh_timer=&refresh,*s_touch_read_timer=&input;
static bool s_display_idle;
static int s_display_awake_lock=1, locks=1;
static bool dimmed;
static int esp_pm_lock_acquire(int lock) {assert(!locks);locks++;return 0;}
static int esp_pm_lock_release(int lock) {assert(locks==1);locks--;return 0;}
static void apply_brightness(bool idle) {dimmed=idle;}
static void lv_timer_set_period(lv_timer_t *timer,uint32_t period) {timer->period=period;}
static void lv_timer_ready(lv_timer_t *timer) {timer->ready++;}
static lv_timer_t *lv_anim_get_timer(void) {return &animation;}
'''
    delay = text[text.index('      if (delay_ms == 0)'):text.index('    } else {\n      // FAILED')]
    run('display',definitions+mock+function(text,'set_display_idle')+r'''
static uint32_t delay_for(uint32_t delay_ms) {
'''+delay+r'''
return delay_ms;
}
int main(void) {
 set_display_idle(true,false);assert(!locks && dimmed && refresh.period==1000 && input.period==1000 && animation.period==1000);
 assert(delay_for(1)==1000 && delay_for(33)==1000 && delay_for(UINT32_MAX)==1000);
 set_display_idle(true,false);assert(!locks);
 set_display_idle(false,true);assert(locks==1 && !dimmed && refresh.period==33 && input.period==33 && animation.period==33 && input.ready==1 && refresh.ready==1);
 assert(delay_for(0)==1 && delay_for(5)==5 && delay_for(1000)==33);
 set_display_idle(false,true);assert(locks==1);
 puts("PASS: idle/active timers, immediate read/render and balanced sleep locks");
}
''')


def http_tests():
    text = source('components/wifi_connect/src/wifi_connect.c')
    text = text[text.index('static SemaphoreHandle_t s_http_mutex'):text.index('char *wifi_scan_networks')]
    mock = r'''
#define HTTP_METHOD_GET 0
#define HTTP_EVENT_ON_CONNECTED 1
#define HTTP_EVENT_ON_DATA 2
typedef struct client *esp_http_client_handle_t;
typedef struct {int event_id;void *user_data;esp_http_client_handle_t client;int data_len;void *data;} esp_http_client_event_t;
typedef struct {const char *url;int method,timeout_ms;void *crt_bundle_attach;bool disable_auto_redirect;int (*event_handler)(esp_http_client_event_t*);void *user_data;} esp_http_client_config_t;
struct client {esp_http_client_config_t cfg;int status;bool connected,complete;};
static void *esp_crt_bundle_attach;
static int active,opened,performed,scenario;
static bool fail_allocation;
static int64_t esp_timer_get_time(void) {return 0;}
static void *test_realloc(void *p,size_t n) {return fail_allocation?NULL:realloc(p,n);}
#define realloc test_realloc
static esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg) {assert(!active);struct client *p=calloc(1,sizeof(*p));p->cfg=*cfg;active++;return p;}
static int esp_http_client_set_url(esp_http_client_handle_t c,const char *url) {c->cfg.url=url;return 0;}
static int esp_http_client_get_status_code(esp_http_client_handle_t c) {return c->status;}
static int64_t esp_http_client_get_content_length(esp_http_client_handle_t c) {return scenario==1?0:2800;}
static bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c) {return c->complete;}
static int esp_http_client_cleanup(esp_http_client_handle_t c) {assert(active==1);active--;free(c);return 0;}
static int esp_http_client_perform(esp_http_client_handle_t c) {
 performed++;
 if(!c->connected) {c->connected=true;opened++;esp_http_client_event_t e={HTTP_EVENT_ON_CONNECTED,c->cfg.user_data,c,0,NULL};c->cfg.event_handler(&e);}
 if((scenario==2 && performed==2) || (scenario==5 && performed>=2))return ESP_ERR_HTTP_CONNECTION_CLOSED;
 c->status=(scenario==3 && performed==1)?429:200;c->complete=scenario!=4;
 char chunk[700];memset(chunk,'x',sizeof(chunk));
 for(int i=0;i<4;i++){esp_http_client_event_t e={HTTP_EVENT_ON_DATA,c->cfg.user_data,c,sizeof(chunk),chunk};c->cfg.event_handler(&e);}
 if(scenario==1)c->connected=false; // server Connection: close
 return 0;
}
'''
    run('http',mock+text+r'''
static void check_batch(int kind,int expected_opens,int expected_calls) {
 scenario=kind;opened=performed=0;
 const char *urls[]={"https://example.test/a","https://example.test/b"};char *buffers[2];int results[2];
 int err=wifi_perform_http_get_batch(urls,buffers,results,2);
 assert(active==0 && !mutex_held && opened==expected_opens && performed==expected_calls);
 if(kind==3) {assert(err==ESP_FAIL && results[0]==ESP_FAIL && !buffers[0] && results[1]==0);}
 else if(kind==5) {assert(err!=0 && results[0]==0 && results[1]!=0 && buffers[0] && !buffers[1]);}
 else if(kind==4) {assert(err!=0 && !buffers[0] && !buffers[1]);}
 else {assert(!err && !results[0] && !results[1] && strlen(buffers[0])==2800 && strlen(buffers[1])==2800);}
 free(buffers[0]);free(buffers[1]);
}
int main(void) {
 check_batch(0,1,2);check_batch(1,2,2);check_batch(2,2,3);check_batch(3,2,2);check_batch(4,2,2);check_batch(5,2,3);
 scenario=0;char *response=(char*)1;
 fail_allocation=true;assert(wifi_perform_http_get("https://example.test",&response)==ESP_ERR_NO_MEM && !response && !active && !mutex_held);fail_allocation=false;
 assert(wifi_perform_http_get(NULL,&response)==ESP_ERR_INVALID_ARG && !response);
 mutex_available=false;assert(wifi_perform_http_get("https://example.test",&response)==ESP_ERR_TIMEOUT && !response);mutex_available=true;
 assert(wifi_perform_http_get("https://example.test",&response)==0 && strlen(response)==2800 && !active);free(response);
 for(int i=0;i<50;i++)check_batch(0,1,2);
 puts("PASS: HTTPS reuse/close/retry, partial results, 429, buffering, OOM and cleanup");
}
''')



def stock_tests():
    text = source('components/stock_monitor/src/stock_monitor.c')
    header = source('components/stock_monitor/include/stock_monitor.h')
    types = header[header.index('typedef struct'):header.index('} stock_values_t;') + len('} stock_values_t;')]
    sdk = os.environ.get('IDF_PATH')
    if not sdk:
        sdk = json.loads((ROOT/'build/project_description.json').read_text())['idf_path']
    cjson = Path(sdk)/'components/json/cJSON'
    definitions = re.search(r'#define FINNHUB_URL_TEMPLATE[^\n]*\n[^\n]*',text)[0]
    functions = ''.join(function(text,name) for name in ['get_json_double','stock_build_url','stock_parse_price','stock_fetch_price','stock_monitor_refresh'])
    mock = r'''
#include "cJSON.h"
#define CONFIG_FINNHUB_API_KEY "test-token"
static char s_stock_symbol_1[16]="qcom",s_stock_symbol_2[16]="SPY";
static SemaphoreHandle_t stock_mutex=(void *)1;
static stock_values_t stock_cache;
static int64_t s_last_update_ts;
static int mode;
static int64_t esp_timer_get_time(void) {return 123;}
static esp_err_t wifi_perform_http_get(const char *url,char **out) {*out=strdup("{\"c\":10}");return 0;}
static esp_err_t wifi_perform_http_get_batch(const char *const *urls,char **out,int *results,size_t count) {
 assert(count==2 && strstr(urls[0],"symbol=QCOM") && strstr(urls[1],"symbol=SPY"));
 out[0]=strdup("{\"c\":10,\"t\":123}");out[1]=strdup(mode==1?"bad json":"{\"c\":20}");results[0]=results[1]=0;
 if(mode==2)strcpy(s_stock_symbol_1,"AAPL");
 return 0;
}
'''
    main = r'''
int main(void) {
 stock_data_t quote={0};char url[256];
 assert(stock_build_url("qcom",url,sizeof(url))==0 && strstr(url,"symbol=QCOM"));
 assert(stock_build_url("",url,sizeof(url))==ESP_ERR_INVALID_ARG);
 assert(stock_build_url("QCOM",url,5)==ESP_ERR_INVALID_SIZE);
 assert(stock_parse_price("QCOM","bad json",&quote)!=0);
 assert(stock_parse_price("QCOM","{\"c\":0}",&quote)!=0);
 assert(stock_parse_price("QCOM","{\"c\":-1}",&quote)!=0);
 assert(stock_parse_price("QCOM","{\"c\":10,\"t\":123}",&quote)==0 && quote.current_price==10 && quote.timestamp==123);
 assert(stock_fetch_price("qcom",&quote)==0 && quote.current_price==10);
 assert(stock_monitor_refresh()==0 && stock_cache.stock_1.current_price==10 && stock_cache.stock_2.current_price==20);
 mode=1;stock_cache.stock_2.current_price=99;assert(stock_monitor_refresh()==0 && stock_cache.stock_2.current_price==99 && !mutex_held);
 mode=2;stock_cache.stock_1.current_price=88;assert(stock_monitor_refresh()==0 && stock_cache.stock_1.current_price==88 && stock_cache.stock_2.current_price==20);
 puts("PASS: quote validation, uppercase URLs, partial cache updates and concurrent symbol changes");
}
'''
    run('stock','#include "cJSON.h"\n'+types+'\n'+definitions+'\n'+mock+functions+main,['-I'+str(cjson),cjson/'cJSON.c'])


def dashboard_tests():
    ui = source('main/ui_updater.c')
    sensor = function(ui, 'update_sensor_ui')
    color = function(ui, 'update_stock_market_ui')
    labels = sorted(set(re.findall(r'\bui_\w+', sensor + color)) |
                    {'ui_LabelStock1Price', 'ui_LabelStock1Change'})
    declarations = '\n'.join(f'static lv_obj_t object_{label};\nstatic lv_obj_t *{label}=&object_{label};' for label in labels)
    fields = sorted(set(re.findall(r'data->(\w+)', sensor)))
    sensor_type = 'typedef struct {' + ''.join(('int' if f == 'iaq_accuracy' else 'float') + ' ' + f + ';' for f in fields) + '} bsec_values_t;\n'
    mock = r"""
#define LV_PART_MAIN 1
#define LV_STATE_DEFAULT 0
#define LV_PART_KNOB 2
#define LV_ANIM_OFF 0
#define LV_OBJ_FLAG_HIDDEN 1
typedef uint32_t lv_color_t;
typedef struct {lv_color_t color,bg;char text[64];} lv_obj_t;
static int style_writes;
static bool valid_clock=true;
static time_t wall;
static int64_t esp_timer_get_time(void) {return 0;}
static bool time_manager_has_valid_time(void) {return valid_clock;}
static lv_color_t lv_color_hex(uint32_t rgb) {return rgb;}
static bool lv_color_eq(lv_color_t a,lv_color_t b) {return a==b;}
static lv_color_t lv_obj_get_style_text_color(lv_obj_t *label,int part) {assert(label && part==LV_PART_MAIN);return label->color;}
static void lv_obj_set_style_text_color(lv_obj_t *label,lv_color_t c,int part) {assert(label && part==(LV_PART_MAIN|LV_STATE_DEFAULT));style_writes++;label->color=c;}
static const char *lv_label_get_text(lv_obj_t *label) {return label->text;}
static void lv_label_set_text(lv_obj_t *label,const char *text) {snprintf(label->text,sizeof(label->text),"%s",text);}
static lv_color_t lv_obj_get_style_bg_color(lv_obj_t *label,int part) {return label->bg;}
static void lv_obj_set_style_bg_color(lv_obj_t *label,lv_color_t color,int part) {label->bg=color;}
static void lv_slider_set_value(lv_obj_t *label,int value,int anim) {}
static void lv_obj_add_flag(lv_obj_t *label,int flag) {}
static void lv_obj_clear_flag(lv_obj_t *label,int flag) {}
static void lv_obj_set_x(lv_obj_t *label,int x) {}
static time_t fake_time(time_t *out) {if(out)*out=wall;return wall;}
// Supply a fixed local calendar for date-format tests, independent of host TZ.
#define time fake_time
#define localtime_r gmtime_r
"""
    helpers = function(source('components/time_manager/time_manager.c'), 'time_manager_get_new_york_time')
    helpers += function(source('components/stock_monitor/src/stock_monitor.c'), 'stock_monitor_is_market_open')
    helpers += function(source('components/time_manager/time_manager.c'), 'time_manager_get_date')
    helpers += function(ui, 'set_label_text_if_changed') + sensor + color
    main = r"""
static void check_colors(uint32_t expected) {
 assert(ui_LabelStock1Symbol->color==expected && ui_LabelStock2Symbol->color==expected);
 assert(ui_LabelStock1SymbolEx->color==expected && ui_LabelStock2SymbolEx->color==expected);
 assert(ui_LabelStock1Price->color==0x123456 && ui_LabelStock1Change->color==0xFF0000);
}
int main(void) {
 char date[32];
"""
    for day, expected in [('2026-10-31T12:00:00','31/10/26'), ('2027-01-01T00:00:00','01/01/27'), ('2028-02-29T00:00:00','29/02/28')]:
        main += f'wall={epoch(day)}LL;time_manager_get_date(date,sizeof(date));assert(strcmp(date,"{expected}")==0);\n'
    main += r"""
 bsec_values_t data={0};
"""
    for humidity, expected in [(0,0),(48.4,48),(48.5,49),(48.9,49),(100,100)]:
        main += f'data.comp_humidity={humidity:.1f}f;update_sensor_ui(&data);assert(strcmp(ui_Label2->text,"{expected}")==0 && data.comp_humidity=={humidity:.1f}f);\n'
    main += r"""
 ui_LabelStock1Price->color=0x123456;ui_LabelStock1Change->color=0xFF0000;
 valid_clock=false;update_stock_market_ui(1768228200);check_colors(0xBBDED6);assert(style_writes==4);
 valid_clock=true;update_stock_market_ui(1768228200);check_colors(0x2ECC71);assert(style_writes==8);
 // Stay green between five-minute quote slots, with no quote/network input.
 update_stock_market_ui(1768228260);check_colors(0x2ECC71);assert(style_writes==8);
 update_stock_market_ui(1768251599);check_colors(0x2ECC71);assert(style_writes==8);
 update_stock_market_ui(1768251600);check_colors(0xBBDED6);assert(style_writes==12);
 ui_LabelStock2SymbolEx=NULL;update_stock_market_ui(1768228200);assert(style_writes==15);
 ui_LabelStock1Symbol=ui_LabelStock2Symbol=ui_LabelStock1SymbolEx=NULL;
 update_stock_market_ui(1768251600);assert(style_writes==15);
 puts("PASS: short dates, integer humidity, ticker colors, unchanged/null labels");
}
"""
    # Guard integration: market styling must not wait for a quote-cache update.
    callback = function(ui, 'ui_update_timer_cb')
    assert callback.index('update_stock_market_ui(now_epoch)') < callback.index('stock_get_last_updated_time()')
    assert 'update_stock_market_ui(time(NULL))' in function(ui, 'ui_updater_init')
    run('dashboard', mock + declarations + sensor_type + helpers + main)

if __name__ == '__main__':
    schedule_tests()
    display_tests()
    http_tests()
    stock_tests()
    dashboard_tests()
