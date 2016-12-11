#include <stdbool.h>
#include <esp_log.h>
#include <esp_system.h>
#include <duktape.h>
#include <stdlib.h>
#include <assert.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include "modules.h"
#include "esp32_duktape/module_fs.h"
#include "esp32_duktape/module_gpio.h"
#include "esp32_duktape/module_timers.h"
#include "esp32_duktape/module_rmt.h"
#include "esp32_duktape/module_http.h"
#include "esp32_duktape/module_wifi.h"
#include "esp32_duktape/module_partitions.h"
#include "esp32_duktape/module_os.h"
#include "esp32_mongoose.h"
#include "duktape_utils.h"
#include "duk_trans_socket.h"
#include "sdkconfig.h"

static char tag[] = "modules";

/**
 * The native Console.log() static function.
 */
static duk_ret_t js_console_log(duk_context *ctx) {
	ESP_LOGD(tag, "js_console_log called");
	switch(duk_get_type(ctx, -1)) {
		case DUK_TYPE_STRING: {
			esp32_duktape_console(duk_get_string(ctx, -1));
			break;
		}

		default: {
			duk_to_string(ctx, -1);
			esp32_duktape_console(duk_get_string(ctx, -1));
			break;
		}
	}
  return 0;
} // js_console_log


typedef struct {
	char *id;
	duk_c_function func;
	int paramCount;
} functionTableEntry_t;


functionTableEntry_t functionTable[] = {
		{"startMongoose",          js_startMongoose, 1},
		{"serverResponseMongoose", js_serverResponseMongoose, 3},
		// Must be last entry
		{NULL, NULL, 0 }
};


/**
 * Retrieve a native function reference by name.
 * ESP32.getNativeFunction(nativeFunctionID)
 * The input stack contains:
 * [ 0] - String - nativeFunctionID - A string name that is used to lookup a function handle.
 */
static duk_ret_t js_esp32_getNativeFunction(duk_context *ctx) {
	ESP_LOGD(tag, ">> js_esp32_getNativeFunction");
	// Check that the first parameter is a string.
	if (duk_is_string(ctx, 0)) {
		const char *nativeFunctionID = duk_get_string(ctx, 0);
		ESP_LOGD(tag, "- nativeFunctionId that we are looking for is \"%s\"", nativeFunctionID);

		// Lookup the handler function in a table.
		functionTableEntry_t *ptr = functionTable;
		while (ptr->id != NULL) {
			if (strcmp(nativeFunctionID, ptr->id) == 0) {
				break;
			}
			ptr++; // Not found yet, let's move to the next entry
		} // while we still have entries in the table
		// If we found an entry, then set it as the return, otherwise return null.
		if (ptr->id != NULL) {
			duk_push_c_function(ctx, ptr->func, ptr->paramCount);
		} else {
			ESP_LOGD(tag, "No native found found called %s", nativeFunctionID);
			duk_push_null(ctx);
		}
	} else {
		ESP_LOGD(tag, "No native function id supplied");
		duk_push_null(ctx);
	}
	// We will have either pushed null or a function reference onto the stack.
	ESP_LOGD(tag, "<< js_esp32_getNativeFunction");
	return 1;
} // js_esp32_getNativeFunction


typedef struct {
	char *levelString;
	esp_log_level_t level;
} level_t;
static level_t levels[] = {
		{"none", ESP_LOG_NONE},
		{"error", ESP_LOG_ERROR},
		{"warn", ESP_LOG_WARN},
		{"info", ESP_LOG_INFO},
		{"debug", ESP_LOG_DEBUG},
		{"verbose", ESP_LOG_VERBOSE},
		{NULL, 0}
};
/**
 * Set the debug log level.
 * [0] - tag - The tag that we are setting the log level on.  Can be "*" for
 *             all tags.
 * [1] - level - The level we are setting for this tage.  Choices are:
 *               * none
 *               * error
 *               * warn
 *               * info
 *               * debug
 *               * verbose
 */
static duk_ret_t js_esp32_setLogLevel(duk_context *ctx) {
	char *tagToChange;
	char *levelString;
	tagToChange = (char *)duk_get_string(ctx, -2);
	levelString = (char *)duk_get_string(ctx, -1);
	ESP_LOGD(tag, "Setting a new log level to be tag: \"%s\", level: \"%s\"", tagToChange, levelString);
	level_t *pLevels = levels;
	while(pLevels->levelString != NULL) {
		if (strcmp(pLevels->levelString, levelString) == 0) {
			break;
		}
		pLevels++;
	}
	if (pLevels->levelString != NULL) {
		esp_log_level_set(tagToChange, pLevels->level);
	}
	return 0;
} // js_esp32_setLogLevel


/**
 * Load a file using the POSIX file I/O functions.
 * [0] - path - The name of the file to load.
 *
 * The String representation of the file will remain on the stack at the end.
 *
 * The high level algorithm is:
 * 1. Determine file size.
 * 2. Allocate a buffer to hold the file.
 * 3. Open the file.
 * 4. Read the whole file into the buffer.
 * 5. Cleanup.
 * 6. Put the buffer as a string onto the stack for return.
 *
 */
static duk_ret_t js_esp32_loadFile(duk_context *ctx) {
	const char *path = duk_get_string(ctx, -1);
	struct stat statBuf;
	int rc = stat(path, &statBuf);
	if (rc < 0) {
		ESP_LOGD(tag, "js_esp32_loadFile: stat() %d %s", errno, strerror(errno));
		return 0;
	}
	int fd = open(path, O_RDWR);
	if (fd < 0) {
		ESP_LOGD(tag, "js_esp32_loadFile: open() %d %s", errno, strerror(errno));
		return 0;
	}
	char *data = malloc(statBuf.st_size);
	ssize_t sizeRead = read(fd, data, statBuf.st_size);
	if (sizeRead < 0) {
		ESP_LOGD(tag, "js_esp32_loadFile: read() %d %s", errno, strerror(errno));
		free(data);
		close(fd);
		return 0;
	}
	close(fd); // Close the open file, we don't need it anymore.
	duk_push_lstring(ctx, data, sizeRead); // Push the data onto the stack.
	free(data); // Release the dynamically read data as it is on the stack now.
	ESP_LOGD(tag, "Read file %s of length %d", path, sizeRead);
	return 1;
} // js_esp32_loadFile


// Ask JS to perform a gabrage collection.
static duk_ret_t js_esp32_gc(duk_context *ctx) {
	duk_gc(ctx, 0);
	return 0;
} // js_esp32_gc


/**
 * Reset the duktape environment by flagging a request to reset.
 */
static duk_ret_t js_esp32_reset(duk_context *ctx) {
	esp32_duktape_set_reset(1);
	return 0;
} // js_esp32_reset


/**
 * ESP32.getState()
 * Return an object that describes the state of the ESP32 environment.
 * - heapSize - The available heap size.
 */
static duk_ret_t js_esp32_getState(duk_context *ctx) {
	// [0] - New object
	duk_push_object(ctx); // Create new getState object

	// [0] - New object
	// [1] - heap size
	duk_push_number(ctx, (double)esp_get_free_heap_size());

	// [0] - New object
	duk_put_prop_string(ctx, -2, "heapSize"); // Add heapSize to new getState

	return 1;
} // js_esp32_getState


/**
 * Attach the debugger.
 */
static duk_ret_t js_esp32_debug(duk_context *ctx) {
	ESP_LOGD(tag, ">> js_esp32_debug");
	duk_trans_socket_init();
	duk_trans_socket_waitconn();
	ESP_LOGD(tag, "Debugger reconnected, call duk_debugger_attach()");

	duk_debugger_attach(ctx,
		duk_trans_socket_read_cb,
		duk_trans_socket_write_cb,
		duk_trans_socket_peek_cb,
		duk_trans_socket_read_flush_cb,
		duk_trans_socket_write_flush_cb,
		NULL,
		NULL);
	ESP_LOGD(tag, "<< js_esp32_debug");
	return 0;
} // js_esp32_debug


/**
 * Write a log record to the debug output stream.  Exposed
 * as the global log("message").
 */
static duk_ret_t js_global_log(duk_context *ctx) {
	ESP_LOGD("debug", "%s", duk_get_string(ctx, -1));
	return 0;
} // js_global_log


/**
 * Define the static module called "ModuleConsole".
 */
static void ModuleConsole(duk_context *ctx) {

	duk_push_global_object(ctx);
	// [0] Global Object

	duk_push_c_function(ctx, js_global_log, 1);
	// [0] Global Object
	// [1] C Function - js_global_log

	duk_put_prop_string(ctx, -2, "log"); // Add log to new console
	// [0] Global Object

	duk_push_object(ctx); // Create new console object
	// [0] Global Object
	// [1] New object

	duk_push_c_function(ctx, js_console_log, 1);
	// [0] Global Object
	// [1] New object
	// [2] c-function - js_console_log

	duk_put_prop_string(ctx, -2, "log"); // Add log to new console
	// [0] Global Object
	// [1] New object

	duk_put_prop_string(ctx, -2, "console"); // Add console to global
	// [0] Global Object

	duk_pop(ctx);
	// <stack empty>
} // ModuleConsole


/**
 * Register the ESP32 module with its functions.
 */
static void ModuleESP32(duk_context *ctx) {
	duk_push_global_object(ctx);
	// [0] - Global object

	duk_push_object(ctx); // Create new ESP32 object
	// [0] - Global object
	// [1] - New object

	duk_push_c_function(ctx, js_esp32_reset, 0);
	// [0] - Global object
	// [1] - New object
	// [2] - c-function - js_esp32_reset

	duk_put_prop_string(ctx, -2, "reset"); // Add reset to new ESP32
	// [0] - Global object
	// [1] - New object

	duk_push_c_function(ctx, js_esp32_getState, 0);
	// [0] - Global object
	// [1] - New object
	// [2] - c-function - js_esp32_getState

	duk_put_prop_string(ctx, -2, "getState"); // Add reset to new ESP32
	// [0] - Global object
	// [1] - New object

	duk_push_c_function(ctx, js_esp32_getNativeFunction, 1);
	// [0] - Global object
	// [1] - New object
	// [2] - c-function - js_esp32_getNativeFunction

	duk_put_prop_string(ctx, -2, "getNativeFunction"); // Add getNativeFunction to new ESP32
	// [0] - Global object
	// [1] - New object

	duk_push_c_function(ctx, js_esp32_debug, 0);
	// [0] - Global object
	// [1] - New object
	// [2] - c-function - js_esp32_debug

	duk_put_prop_string(ctx, -2, "debug"); // Add debug to new ESP32
	// [0] - Global object
	// [1] - New object

	duk_push_c_function(ctx, js_esp32_setLogLevel, 2);
	// [0] - Global object
	// [1] - New object
	// [2] - c-function - js_esp32_setLogLevel

	duk_put_prop_string(ctx, -2, "setLogLevel"); // Add setLogLevel to new ESP32
	// [0] - Global object
	// [1] - New object

	duk_push_c_function(ctx, js_esp32_gc, 0);
	// [0] - Global object
	// [1] - New object
	// [2] - c-function - js_esp32_gc

	duk_put_prop_string(ctx, -2, "gc"); // Add gc to new ESP32
	// [0] - Global object
	// [1] - New object

	duk_push_c_function(ctx, js_esp32_loadFile, 1);
	// [0] - Global object
	// [1] - New object
	// [2] - c-function - js_esp32_loadFile

	duk_put_prop_string(ctx, -2, "loadFile"); // Add loadFile to new ESP32
	// [0] - Global object
	// [1] - New object

	duk_put_prop_string(ctx, -2, "ESP32"); // Add ESP32 to global
	// [0] - Global object

	duk_pop(ctx);
	// <Empty stack>
} // ModuleESP32


/**
 * Register the static modules.  These are modules that will ALWAYS
 * bein the global address space/scope.
 */
void registerModules(duk_context *ctx) {
	duk_idx_t top = duk_get_top(ctx);
	ModuleConsole(ctx);
	assert(top == duk_get_top(ctx));
	ModuleESP32(ctx);
	assert(top == duk_get_top(ctx));
	ModuleFS(ctx);
	assert(top == duk_get_top(ctx));
	ModuleGPIO(ctx);
	assert(top == duk_get_top(ctx));
	ModuleTIMERS(ctx);
	assert(top == duk_get_top(ctx));
	ModuleWIFI(ctx);
	assert(top == duk_get_top(ctx));
	ModuleRMT(ctx);
	assert(top == duk_get_top(ctx));
	ModuleHTTP(ctx);
	assert(top == duk_get_top(ctx));
	ModulePARTITIONS(ctx);
	assert(top == duk_get_top(ctx));
	ModuleMONGOOSE(ctx);
	assert(top == duk_get_top(ctx));
	ModuleOS(ctx);
	assert(top == duk_get_top(ctx));
} // End of registerModules
