/* main.c - Application main entry point */

/*
 * Copyright (c) 2026 Silicon Labs
 *
 * SPDX-License-Identifier: Apache-2.0
 */

//! **Find Me (Step 4: Locator) - State Transition Diagram**
//! ```mermaid
//! stateDiagram-v2

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/types.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>

#include <zephyr/drivers/led.h>
#include <zephyr/drivers/pwm.h>

#include <zephyr/input/input.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

// APP DATA -------------------------------------------------------------------

// App states
#define APP_STATE_LIST(X)        \
	X(APP_STATE_IDLE)            \
	X(APP_STATE_ADVERTISING)     \
	X(APP_STATE_CONNECTED)       \
	X(APP_STATE_SCANNING)        \
	X(APP_STATE_SET_CONNECTING)  \
	X(APP_STATE_DISCOVERING_IAS) \
	X(APP_STATE_DISCOVERING_ALC) \
	X(APP_STATE_SET_WRITING_ALC) \
	X(APP_STATE_SET_DISCONNECTING)

// App events
#define APP_EVENT_LIST(X)                     \
	X(APP_EVENT_START)                        \
	X(APP_EVENT_PRESSED_SW0)                  \
	X(APP_EVENT_PRESSED_SW1)                  \
	X(APP_EVENT_CONNECTED_PERIPHERAL)         \
	X(APP_EVENT_DISCONNECTED_PERIPHERAL)      \
	X(APP_EVENT_SCANNED_TARGET)               \
	X(APP_EVENT_CONNECTED_FAIL)               \
	X(APP_EVENT_CONNECTED_CENTRAL)            \
	X(APP_EVENT_DISCONNECTED_CENTRAL)         \
	X(APP_EVENT_DISCOVER_SERVICE_FAIL)        \
	X(APP_EVENT_DISCOVER_SERVICE_OK)          \
	X(APP_EVENT_DISCOVER_CHARACTERISTIC_FAIL) \
	X(APP_EVENT_DISCOVER_CHARACTERISTIC_OK)   \
	X(APP_EVENT_TIMEOUT)

// App alert levels
#define APP_ALERT_LEVEL_LIST(X)     \
	X(APP_ALERT_LEVEL_TARGET_NONE)  \
	X(APP_ALERT_LEVEL_TARGET_MILD)  \
	X(APP_ALERT_LEVEL_TARGET_HIGH)  \
	X(APP_ALERT_LEVEL_LOCATOR_NONE) \
	X(APP_ALERT_LEVEL_LOCATOR_MILD) \
	X(APP_ALERT_LEVEL_LOCATOR_HIGH)

// Create state and event enums
#define GENERATE_ENUM(name) name,

typedef enum
{
	APP_STATE_LIST(GENERATE_ENUM)
		APP_STATE_MAX
} app_state_t;

typedef enum
{
	APP_EVENT_LIST(GENERATE_ENUM)
		APP_EVENT_MAX
} app_event_t;

typedef enum
{
	APP_ALERT_LEVEL_LIST(GENERATE_ENUM)
		APP_ALERT_LEVEL_MAX
} app_alert_level_t;

#undef GENERATE_ENUM

// Create state and evevnt strings
#define GENERATE_STRING(name) #name,

static const char *app_state_strings[APP_STATE_MAX] = {
	APP_STATE_LIST(GENERATE_STRING)};

static const char *app_event_strings[APP_EVENT_MAX] = {
	APP_EVENT_LIST(GENERATE_STRING)};

static const char *app_alert_level_strings[APP_ALERT_LEVEL_MAX] = {
	APP_ALERT_LEVEL_LIST(GENERATE_STRING)};

#undef GENERATE_STRING

// App state
static app_state_t app_state = APP_STATE_IDLE;

// App event message queue
#define APP_EVENT_QUEUE_DEPTH 8
K_MSGQ_DEFINE(app_event_queue, sizeof(app_event_t), APP_EVENT_QUEUE_DEPTH, 4);

// Alert level
static volatile app_alert_level_t app_alert_level = APP_ALERT_LEVEL_TARGET_NONE;

// App work item
static struct k_work app_work;

// OUTPUT DATA ----------------------------------------------------------------
// Output state
static bool output_is_on = false;
// Output work item
static struct k_work_delayable output_work;
// Note frequencies
#define NOTE_C4_HZ 262U
#define NOTE_C5_HZ 523U
// Note periods
#define NOTE_C4_PERIOD_NS (NSEC_PER_SEC / NOTE_C4_HZ)
#define NOTE_C5_PERIOD_NS (NSEC_PER_SEC / NOTE_C5_HZ)
#define NOTE_NONE_PERIOD_NS NOTE_C4_PERIOD_NS
// Output configuration
typedef struct
{
	bool red;
	bool green;
	bool blue;
	uint32_t piezo;
	uint32_t on_ms;
	uint32_t off_ms;
} output_config_t;
const output_config_t output_config[] = {
	{false, false, true, 0, 50, 1450},				   // APP_ALERT_LEVEL_TARGET_NONE
	{false, true, false, NOTE_C4_PERIOD_NS, 50, 1450}, // APP_ALERT_LEVEL_TARGET_MILD
	{true, false, false, NOTE_C5_PERIOD_NS, 50, 1450}, // APP_ALERT_LEVEL_TARGET_HIGH
	{false, false, true, 0, 50, 450},				   // APP_ALERT_LEVEL_LOCATOR_NONE
	{false, true, false, 0, 50, 450},				   // APP_ALERT_LEVEL_LOCATOR_MILD
	{true, false, false, 0, 50, 450},				   // APP_ALERT_LEVEL_LOCATOR_HIGH
	{true, true, true, 0, 50, 1450},				   // APP_STATE_IDLE
};
#define OUTPUT_CONFIG_APP_STATE_IDLE (APP_ALERT_LEVEL_LOCATOR_HIGH + 1)
// Piezo PWM
static const struct pwm_dt_spec output_piezo = PWM_DT_SPEC_GET(DT_ALIAS(piezo));
// LED GPIOs
static const struct led_dt_spec output_red = LED_DT_SPEC_GET(DT_ALIAS(red));
static const struct led_dt_spec output_green = LED_DT_SPEC_GET(DT_ALIAS(green));
static const struct led_dt_spec output_blue = LED_DT_SPEC_GET(DT_ALIAS(blue));

// BLUETOOTH DATA -------------------------------------------------------------

// BT advertising parameters
static const struct bt_le_adv_param *ad_param = BT_LE_ADV_PARAM(
	BT_LE_ADV_OPT_CONN,
	4400, /* 2750ms */
		  /* 3000ms average, 500ms spread */
	5200, /* 3250ms */
	NULL);

// BT advertising data (22 characters max for device name)
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL, BT_UUID_16_ENCODE(BT_UUID_IAS_VAL)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

/* Scan parameters, passive all data is in advertising data */
static struct bt_le_scan_param scan_param = {
	.type = BT_LE_SCAN_TYPE_PASSIVE,
	.options = BT_LE_SCAN_OPT_NONE,
	.interval = BT_GAP_SCAN_FAST_INTERVAL, /* 60ms */
	.window = 0x0048,					   /* 45ms (75% duty)  */
	.timeout = 0,
};

// Scan flags (can remove SCAN_FLAG_NAME from ad_parse_match to look only for IAS)
#define AD_PARSE_FLAG_IAS 0x1
#define AD_PARSE_FLAG_NAME 0x2
static const uint8_t ad_parse_match = (AD_PARSE_FLAG_IAS | AD_PARSE_FLAG_NAME);
static uint8_t ad_parse_flags;
// Found target device
static bt_addr_le_t target_addr;
// Size of target stacks
#define TARGET_STACK_MAX 16
// Target ignore stack
// (these devices have been connected to and should be ignored in scans)
static bt_addr_le_t target_ignore_stack[TARGET_STACK_MAX];
static uint8_t target_ignore_stack_count;

// Discovery UUIDs
static struct bt_uuid_16 bt_uuid_ias = BT_UUID_INIT_16(BT_UUID_IAS_VAL);
static struct bt_uuid_16 bt_uuid_alc = BT_UUID_INIT_16(BT_UUID_ALERT_LEVEL_VAL);
// Discovery parameters
static struct bt_gatt_discover_params discover_params_service;
static struct bt_gatt_discover_params discover_params_characteristic;
// GATT handles
static uint16_t handle_service_start;
static uint16_t handle_service_end;
static uint16_t handle_characteristic_value;
// Write timeout period
#define TIMEOUT_MS 100
// Write timeout delayable work
static struct k_work_delayable bt_timeout_work;

// Current connection
static struct bt_conn *bt_current_conn = NULL;

// APP INLINE CODE ------------------------------------------------------------

// Post event to application
static inline void app_event_queue_add(app_event_t evt)
{
	int err;

	// Add event to queue
	err = k_msgq_put(&app_event_queue, &evt, K_NO_WAIT);
	// Failed ?
	if (err)
	{
		printk("k_msgq_put(%d) = %d\n", evt, err);
	}
	// Success ?
	else
	{
		err = k_work_submit(&app_work);
		// Failed ?
		if (err < 0)
		{
			printk("k_work_submit(app_work) = %d\n", err);
		}
	}
}

// Set state
static inline void app_state_set(app_state_t state)
{
	// Go to state
	app_state = state;
	printk("  app_state_set(%s)\n", app_state_strings[app_state]);
}

// Set alert level
static inline void app_alert_level_set(app_alert_level_t alert_level)
{
	// Go to alert level
	app_alert_level = alert_level;
	printk("  app_alert_level_set(%s)\n", app_alert_level_strings[app_alert_level]);
}

// INPUT CODE -----------------------------------------------------------------

// Callback function (outside interrupt context)
static void input_cb(struct input_event *evt, void *user_data)
{

	// End of input (the input system can deliver coherent batches of events) ?
	if (evt->sync)
	{
		// Key event type ?
		if (evt->type == INPUT_EV_KEY)
		{
			// Which key ?
			switch (evt->code)
			{
			// Key 0 ?
			case INPUT_KEY_0:
				// Pressed ?
				if (evt->value)
				{
					app_event_queue_add(APP_EVENT_PRESSED_SW0);
				}
				break;
			// Key 1 ?
			case INPUT_KEY_1:
				// Pressed ?
				if (evt->value)
				{
					app_event_queue_add(APP_EVENT_PRESSED_SW1);
				}
				break;
			// Others
			default:
				break;
			}
		}
	}
}

// Define button callback function
INPUT_CALLBACK_DEFINE(NULL, input_cb, NULL);

// OUTPUT CODE ----------------------------------------------------------------

static void output_piezo_set(const uint32_t period)
{
	if (pwm_is_ready_dt(&output_piezo))
	{
		// Off ?
		if (period == 0)
			pwm_set_dt(&output_piezo, NOTE_NONE_PERIOD_NS, 0);
		// On ?
		else
			pwm_set_dt(&output_piezo, period, (period / 2U));
	}
}

static void output_led_set(const struct led_dt_spec *led_spec, const bool led_on)
{
	if (led_is_ready_dt(led_spec))
	{
		if (led_on)
			led_on_dt(led_spec);
		else
			led_off_dt(led_spec);
	}
}

// Output work handler
static void output_work_handler(struct k_work *work)
{
	uint32_t output_config_index;

	// Get output config index
	if (APP_STATE_IDLE == app_state)
		output_config_index = OUTPUT_CONFIG_APP_STATE_IDLE;
	else
		output_config_index = app_alert_level;
	// Toggle output
	output_is_on = !output_is_on;
	// Turn outputs on ?
	if (output_is_on)
	{
		output_piezo_set(output_config[output_config_index].piezo);
		output_led_set(&output_red, output_config[output_config_index].red);
		output_led_set(&output_green, output_config[output_config_index].green);
		output_led_set(&output_blue, output_config[output_config_index].blue);
		// Reschedule after on period
		k_work_schedule(&output_work, K_MSEC(output_config[output_config_index].on_ms));
	}
	// Turn outputs off ?
	else
	{
		output_piezo_set(0);
		output_led_set(&output_red, false);
		output_led_set(&output_green, false);
		output_led_set(&output_blue, false);
		// Reschedule after off period
		k_work_schedule(&output_work, K_MSEC(output_config[output_config_index].off_ms));
	}
}

// APP TARGET STACK CODE ------------------------------------------------------

static void target_ignore_stack_clear(void)
{
	memset(target_ignore_stack, 0, sizeof(target_ignore_stack));
	target_ignore_stack_count = 0;
}

static bool target_ignore_stack_check(const bt_addr_le_t *addr)
{
	// List is full, fail the check
	if (target_ignore_stack_count >= TARGET_STACK_MAX)
		return false;
	// Address is in list, fail the check
	for (uint8_t i = 0; i < target_ignore_stack_count; i++)
	{
		if (bt_addr_le_cmp(addr, &target_ignore_stack[i]) == 0)
		{
			return false;
		}
	}

	return true;
}

static bool target_ignore_stack_push(const bt_addr_le_t *addr)
{
	// Can add to list ?
	if (target_ignore_stack_check(addr))
	{
		bt_addr_le_copy(&target_ignore_stack[target_ignore_stack_count], addr);
		target_ignore_stack_count++;
		return true;
	}

	return false;
}

static void target_ignore_stack_pop(void)
{
	// Got something in list ?
	if (target_ignore_stack_count > 0)
	{
		target_ignore_stack_count--;
	}
}

// APP BLE HELPER CODE --------------------------------------------------------

static void app_timeout_work_handler(struct k_work *work)
{
	// Raise event
	app_event_queue_add(APP_EVENT_TIMEOUT);
}

static int app_write_alert_level(const uint8_t alert_level)
{
	int err = -ENOTCONN;

	// Have a connection ?
	if (bt_current_conn)
	{
		err = -EINVAL;
		// Have a handle ?
		if (handle_characteristic_value != 0xFFFF)
		{
			// Write without response
			err = bt_gatt_write_without_response(bt_current_conn,
												 handle_characteristic_value,
												 &alert_level,
												 sizeof(alert_level),
												 false); /* unsigned */
			printk("  bt_gatt_write_without_response(%s) = %d = %s\n", app_alert_level_strings[app_alert_level], err, strerror(-err));
		}
	}

	return err;
}

static uint8_t app_discover_characteristic_cb(struct bt_conn *conn,
											  const struct bt_gatt_attr *attr,
											  struct bt_gatt_discover_params *params)
{
	// Found characteristic ?
	if (attr)
	{
		// Extract characteristic handles
		const struct bt_gatt_chrc *chrc = attr->user_data;
		char uuid_string[BT_UUID_STR_LEN];
		bt_uuid_to_str(chrc->uuid, uuid_string, sizeof(uuid_string));
		handle_characteristic_value = chrc->value_handle;
		printk("app_discover_characteristic_cb(%s, %u)\n", uuid_string, handle_characteristic_value);
		// Raise event
		app_event_queue_add(APP_EVENT_DISCOVER_CHARACTERISTIC_OK);
	}
	// End of discovery ?
	else
	{
		printk("app_discover_characteristic_cb(NULL)\n");
		// Raise event
		app_event_queue_add(APP_EVENT_DISCOVER_CHARACTERISTIC_FAIL);
	}

	return BT_GATT_ITER_STOP;
}

static int app_discover_characteristic(const struct bt_uuid *uuid)
{
	int err = -ENOTCONN;

	// Have a connection ?
	if (bt_current_conn)
	{
		err = -EINVAL;
		// Have a uuid ?
		if (uuid != NULL)
		{
			// Reset discovered handles
			handle_characteristic_value = 0xFFFF;
			// Set discovery parameters
			memset(&discover_params_characteristic, 0, sizeof(discover_params_characteristic));
			discover_params_characteristic.uuid = uuid;
			discover_params_characteristic.start_handle = handle_service_start;
			discover_params_characteristic.end_handle = handle_service_end;
			discover_params_characteristic.type = BT_GATT_DISCOVER_CHARACTERISTIC;
			discover_params_characteristic.func = app_discover_characteristic_cb;
			// Start discovery
			err = bt_gatt_discover(bt_current_conn, &discover_params_characteristic);
			// Get uuid as string (for debugging)
			char uuid_str[BT_UUID_STR_LEN];
			bt_uuid_to_str(discover_params_characteristic.uuid, uuid_str, sizeof(uuid_str));
			printk("  bt_gatt_discover(%s, %u, %u) = %d = %s\n", uuid_str, discover_params_characteristic.start_handle, discover_params_characteristic.end_handle, err, strerror(-err));
		}
	}

	return err;
}

static uint8_t app_discover_service_cb(struct bt_conn *conn,
									   const struct bt_gatt_attr *attr,
									   struct bt_gatt_discover_params *params)
{
	// Found service ?
	if (attr)
	{

		// Extract service handles
		const struct bt_gatt_service_val *svc = attr->user_data;
		handle_service_start = attr->handle + 1;
		handle_service_end = svc->end_handle;
		char uuid_string[BT_UUID_STR_LEN];
		bt_uuid_to_str(svc->uuid, uuid_string, sizeof(uuid_string));
		printk("app_discover_service_cb(%s, %u, %u)\n", uuid_string, handle_service_start, handle_service_end);
		// Raise event
		app_event_queue_add(APP_EVENT_DISCOVER_SERVICE_OK);
	}
	// End of discovery ?
	else
	{
		printk("app_discover_service_cb(NULL)\n");
		// Raise event
		app_event_queue_add(APP_EVENT_DISCOVER_SERVICE_FAIL);
	}

	return BT_GATT_ITER_STOP;
}

static int app_discover_service(const struct bt_uuid *uuid)
{
	int err = -ENOTCONN;

	// Have a connection ?
	if (bt_current_conn)
	{
		err = -EINVAL;
		// Have a uuid ?
		if (uuid != NULL)
		{
			// Reset discovered handles
			handle_service_start = 0xFFFF;
			handle_service_end = 0xFFFF;
			// Set discovery parameters
			memset(&discover_params_service, 0, sizeof(discover_params_service));
			discover_params_service.uuid = uuid;
			discover_params_service.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
			discover_params_service.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
			discover_params_service.type = BT_GATT_DISCOVER_PRIMARY;
			discover_params_service.func = app_discover_service_cb;
			// Start discovery
			err = bt_gatt_discover(bt_current_conn, &discover_params_service);
			// Get uuid as string (for debugging)
			char uuid_str[BT_UUID_STR_LEN];
			bt_uuid_to_str(discover_params_service.uuid, uuid_str, sizeof(uuid_str));
			printk("  bt_gatt_discover(%s) = %d = %s\n", uuid_str, err, strerror(-err));
		}
	}

	return err;
}

static int app_disconnect()
{
	int err = -ENOTCONN;

	// Have a connection ?
	if (bt_current_conn)
	{
		char addr_str[BT_ADDR_LE_STR_LEN];
		bt_addr_le_to_str(bt_conn_get_dst(bt_current_conn), addr_str, sizeof(addr_str));
		// Attempt disconnection
		err = bt_conn_disconnect(bt_current_conn, BT_HCI_ERR_LOCALHOST_TERM_CONN);
		printk("  bt_conn_disconnect(%s) = %d = %s\n", addr_str, err, strerror(-err));
	}

	return err;
}

static int app_connect(const bt_addr_le_t *peer)
{
	int err;
	struct bt_conn *temp_conn;

	// Attempt connection to target
	err = bt_conn_le_create(peer,
							BT_CONN_LE_CREATE_PARAM(BT_CONN_LE_OPT_NONE,
													BT_GAP_SCAN_FAST_INTERVAL,
													BT_GAP_SCAN_FAST_WINDOW),
							BT_LE_CONN_PARAM_DEFAULT,
							&temp_conn);
	char addr_str[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(peer, addr_str, sizeof(addr_str));
	printk("  bt_conn_le_create(%s) = %d = %s\n", addr_str, err, strerror(-err));
	// Success, free the temporary connection
	if (!err)
	{
		bt_conn_unref(temp_conn);
		temp_conn = NULL;
	}

	return err;
}

// Sets flags for IAS and matching device name in ad data
static bool app_ad_parse(struct bt_data *data, void *user_data)
{
	switch (data->type)
	{

	case BT_DATA_UUID16_ALL:
	case BT_DATA_UUID16_SOME:
		// Look for immediate alert service in advertised services
		for (uint8_t i = 0; i + 1 < data->data_len; i += 2)
		{
			uint16_t uuid16 = sys_get_le16(&data->data[i]);
			if (uuid16 == BT_UUID_IAS_VAL)
			{
				// Flag we found ias
				ad_parse_flags |= AD_PARSE_FLAG_IAS;
			}
		}
		break;

	case BT_DATA_NAME_COMPLETE:
	case BT_DATA_NAME_SHORTENED:
		// Is this device's name the same as ours
		size_t target_len = strlen(CONFIG_BT_DEVICE_NAME);
		if (data->data_len == target_len &&
			memcmp(data->data, CONFIG_BT_DEVICE_NAME, target_len) == 0)
		{
			// Flag we found a matching name
			ad_parse_flags |= AD_PARSE_FLAG_NAME;
		}
		break;

	default:
		break;
	}

	return true; /* keep parsing remaining AD elements */
}

static void app_scan_recv_cb(const struct bt_le_scan_recv_info *info,
							 struct net_buf_simple *ad)
{
	// Scanning ?
	if (app_state == APP_STATE_SCANNING)
	{
		// This device is connectable ?
		if (info->adv_props & BT_GAP_ADV_PROP_CONNECTABLE)
		{
			// Parse advertising data
			ad_parse_flags = 0;
			bt_data_parse(ad, app_ad_parse, NULL);
			// Is this a device we want to connect to ?
			if (ad_parse_flags == ad_parse_match)
			{
				// OK to connect to this device ?
				if (target_ignore_stack_check(info->addr))
				{
					// Store address for connection
					bt_addr_le_copy(&target_addr, info->addr);
					// Get address as string
					char addr_str[BT_ADDR_LE_STR_LEN];
					bt_addr_le_to_str(&target_addr, addr_str, sizeof(addr_str));
					// Debug
					printk("app_scan_recv_cb(%s)\n", addr_str);
					// Post we scanned a target
					app_event_queue_add(APP_EVENT_SCANNED_TARGET);
				}
			}
		}
	}
}

/* Register the modern scan receive callback */
static struct bt_le_scan_cb scan_cb = {
	.recv = app_scan_recv_cb,
};

static int app_scan_start(void)
{
	int err;

	// Attempt scanning, don't use legacy advertising callback, instead use modern as configured above
	err = bt_le_scan_start(&scan_param, NULL);
	printk("  bt_le_scan_start() = %d = %s\n", err, strerror(-err));

	return err;
}

static int app_scan_stop(void)
{
	int err;

	// Attempt stop scanning
	err = bt_le_scan_stop();
	printk("  bt_le_scan_stop() = %d = %s\n", err, strerror(-err));

	return err;
}

// Alert Level write callback
static ssize_t app_alert_level_written_cb(struct bt_conn *conn,
										  const struct bt_gatt_attr *attr,
										  const void *buf, uint16_t len,
										  uint16_t offset, uint8_t flags)
{
	// Only one byte, check offset is not set
	if (offset != 0)
	{
		printk("app_alert_level_written_cb(BT_ATT_ERR_INVALID_OFFSET)\n");
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}

	// Check only one byte
	if (len != 1)
	{
		printk("app_alert_level_written_cb(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN)\n");
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	uint8_t alert_level = ((const uint8_t *)buf)[0];
	printk("app_alert_level_written_cb(%s)\n", app_alert_level_strings[alert_level]);

	/* Not locating ? */
	if (app_alert_level <= APP_ALERT_LEVEL_TARGET_HIGH)
	{
		app_alert_level_set(alert_level);
	}

	return len;
}

// BLE Immediate Alert Service definition
BT_GATT_SERVICE_DEFINE(ias_svc,
					   BT_GATT_PRIMARY_SERVICE(BT_UUID_IAS),
					   BT_GATT_CHARACTERISTIC(BT_UUID_ALERT_LEVEL,
											  BT_GATT_CHRC_WRITE_WITHOUT_RESP,
											  BT_GATT_PERM_WRITE,
											  NULL, app_alert_level_written_cb, NULL));

// BLE connected callback (advertising automatically stopped)
static void app_connected_cb(struct bt_conn *conn, uint8_t hci_err)
{
	printk("app_connected_cb(hci_err = 0x%02x = %s)\n", hci_err, bt_hci_err_to_str(hci_err));
	// Success ?
	if (!hci_err)
	{
		// Don't currently have a connection stored ?
		// (shouldn't be possible with connections limited to 1)
		if (bt_current_conn == NULL)
		{
			/* Get connection info */
			struct bt_conn_info info;
			int err = bt_conn_get_info(conn, &info);
			// Failed ?
			if (err)
			{
				printk("  bt_conn_get_info() = %d = %s", err, strerror(-err));
			}
			// Success ?
			else
			{
				// Extract address and convert to string
				const bt_addr_le_t *peer_addr = bt_conn_get_dst(conn);
				char addr_str[BT_ADDR_LE_STR_LEN];
				bt_addr_le_to_str(peer_addr, addr_str, sizeof(addr_str));
				// Peripheral role ?
				if (info.role == BT_CONN_ROLE_PERIPHERAL)
				{
					printk("  role = PERIPHERAL\n");
					printk("  peer_addr = %s\n", addr_str);
					// Save connection
					bt_current_conn = bt_conn_ref(conn);
					// Post we are connected
					app_event_queue_add(APP_EVENT_CONNECTED_PERIPHERAL);
				}
				// Central role ?
				else if (info.role == BT_CONN_ROLE_CENTRAL)
				{
					printk("  role = CENTRAL\n");
					printk("  peer_addr = %s\n", addr_str);
					// Save connection
					bt_current_conn = bt_conn_ref(conn);
					// Add to ignore list so we dont reconnect in future scans
					target_ignore_stack_push(bt_conn_get_dst(conn));
					// Post we are connected
					app_event_queue_add(APP_EVENT_CONNECTED_CENTRAL);
				}
			}
		}
	}
	// Failure
	// Shouldn't happen in peripheral/target mode (but advertising will continue if it happens)
	// May see this in central/locator mode and we'll need to return to scanning
	// Pass an event through anyway let the state machine deal with it (we can't determine the
	// role from parameter data)
	else
	{
		// Post there was a connection failure
		app_event_queue_add(APP_EVENT_CONNECTED_FAIL);
	}
}

// BLE disconnected callback (advertising needs to be restarted, but a bit later)
static void app_disconnected_cb(struct bt_conn *conn, uint8_t hci_err)
{
	printk("app_disconnected_cb(hci_err = 0x%02x = %s)\n", hci_err, bt_hci_err_to_str(hci_err));
	// Is this our stored connection ?
	// (shouldn't be possible to get a different one with connections limited to 1)
	if (bt_current_conn == conn)
	{
		/* Get connection info */
		struct bt_conn_info info;
		int err = bt_conn_get_info(conn, &info);
		// Failed ?
		if (err)
		{
			printk("  bt_conn_get_info() = %d = %s", err, strerror(-err));
		}
		// Success ?
		else
		{
			// Extract address and convert to string
			const bt_addr_le_t *peer_addr = bt_conn_get_dst(conn);
			char addr_str[BT_ADDR_LE_STR_LEN];
			bt_addr_le_to_str(peer_addr, addr_str, sizeof(addr_str));
			// Peripheral role ?
			if (info.role == BT_CONN_ROLE_PERIPHERAL)
			{
				printk("  role = PERIPHERAL\n");
				printk("  peer_addr = %s\n", addr_str);
				// Clear saved connection
				bt_conn_unref(bt_current_conn);
				bt_current_conn = NULL;
				// Post we are disconnected
				app_event_queue_add(APP_EVENT_DISCONNECTED_PERIPHERAL);
			}
			// Central role ?
			else if (info.role == BT_CONN_ROLE_CENTRAL)
			{
				printk("  role = CENTRAL\n");
				printk("  peer_addr = %s\n", addr_str);
				// Clear saved connection
				bt_conn_unref(bt_current_conn);
				bt_current_conn = NULL;
				// Post we are disconnected
				app_event_queue_add(APP_EVENT_DISCONNECTED_CENTRAL);
			}
		}
	}
}

// Define BLE connection state callbacks
BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = app_connected_cb,
	.disconnected = app_disconnected_cb,
};

static int app_advertise_stop(void)
{
	int err;

	// Attempt advertising
	err = bt_le_adv_stop();
	printk("  bt_le_adv_stop() = %d = %s\n", err, strerror(-err));

	return err;
}

static int app_advertise_start(void)
{
	int err;

	// Attempt advertising
	err = bt_le_adv_start(ad_param, ad, ARRAY_SIZE(ad), NULL, 0);
	printk("  bt_le_adv_start() = %d = %s\n", err, strerror(-err));

	return err;
}

// APP MAIN CODE --------------------------------------------------------------

// App event for idle state
static int app_event_idle(app_event_t app_evt)
{
	int err = 0;

	// Valid event ?
	if (app_evt < APP_EVENT_MAX)
	{
		printk("app_event_idle(%s)\n", app_event_strings[app_evt]);
		// Which event ?
		switch (app_evt)
		{
		case APP_EVENT_START:
		case APP_EVENT_PRESSED_SW0:
		case APP_EVENT_PRESSED_SW1:
			// Attempt advertising
			err = app_advertise_start();
			// Success ?
			if (!err)
			{
				// Go to advertising state
				//! IDLE --> ADVERTISING: start
				//! IDLE --> ADVERTISING: pressed_swx
				app_state_set(APP_STATE_ADVERTISING);
			}
			break;
		default:
			printk("  {UNEXPECTED_EVENT}\n");
			break;
		}
	}
	// Invalid event ?
	else
	{
		printk("app_event_idle(%u) {UNKNOWN}\n", app_evt);
	}

	return err;
}

// App event for advertising state
static int app_event_advertising(app_event_t app_evt)
{
	int err = 0;

	// Valid event ?
	if (app_evt < APP_EVENT_MAX)
	{
		printk("app_event_advertising(%s)\n", app_event_strings[app_evt]);
		// Which event ?
		switch (app_evt)
		{
		case APP_EVENT_PRESSED_SW0:
		case APP_EVENT_PRESSED_SW1:
			// Stop advertising
			(void)app_advertise_stop();
			// Clear target lists
			target_ignore_stack_clear();
			// Start scanning
			err = app_scan_start();
			// Success ?
			if (!err)
			{
				// Go to scanning state
				//! ADVERTISING --> SCANNING: pressed_swx
				app_state_set(APP_STATE_SCANNING);
				// Update locator alert level
				if (app_evt == APP_EVENT_PRESSED_SW0)
					app_alert_level_set(APP_ALERT_LEVEL_LOCATOR_MILD);
				else if (app_evt == APP_EVENT_PRESSED_SW1)
					app_alert_level_set(APP_ALERT_LEVEL_LOCATOR_HIGH);
			}
			break;

		case APP_EVENT_CONNECTED_PERIPHERAL:
			// Advertising is automatically stopped, no need to stop it from app
			// Go to connected peripheral state
			//! ADVERTISING --> CONNECTED: connected_peripheral
			app_state_set(APP_STATE_CONNECTED);
			break;

		default:
			printk("  {UNEXPECTED_EVENT}\n");
			break;
		}
	}
	// Invalid event ?
	else
	{
		printk("app_event_advertising(%u) {UNKNOWN}\n", app_evt);
	}

	return err;
}

// App event for connected state
static int app_event_connected(app_event_t app_evt)
{
	int err = 0;

	// Valid event ?
	if (app_evt < APP_EVENT_MAX)
	{
		printk("app_event_connected(%s)\n", app_event_strings[app_evt]);
		// Which event ?
		switch (app_evt)
		{
		case APP_EVENT_DISCONNECTED_PERIPHERAL:
			// Attempt advertising
			err = app_advertise_start();
			// Success ?
			if (!err)
			{
				// Go to advertising state
				//! CONNECTED --> ADVERTISING: disconnected_peripheral
				app_state_set(APP_STATE_ADVERTISING);
			}
			break;
		default:
			printk("  {UNEXPECTED_EVENT}\n");
			break;
		}
	}
	// Invalid event ?
	else
	{
		printk("app_event_connected(%u) {UNKNOWN}\n", app_evt);
	}

	return err;
}

// App event for scanning state
static int app_event_scanning(app_event_t app_evt)
{
	int err = 0;

	// Valid event ?
	if (app_evt < APP_EVENT_MAX)
	{
		printk("app_event_scanning(%s)\n", app_event_strings[app_evt]);
		// Which event ?
		switch (app_evt)
		{
		case APP_EVENT_PRESSED_SW0:
		case APP_EVENT_PRESSED_SW1:
			// Stop scanning
			(void)app_scan_stop();
			// Go to target alert level none
			app_alert_level_set(APP_ALERT_LEVEL_TARGET_NONE);
			// Start advertising
			err = app_advertise_start();
			// Success ?
			if (!err)
			{
				// Go to advertising state
				//! SCANNING --> ADVERTISING: pressed_swx
				app_state_set(APP_STATE_ADVERTISING);
			}
			break;

		case APP_EVENT_SCANNED_TARGET:
			// Stop scanning
			(void)app_scan_stop();
			// Attempt connection to found target address
			err = app_connect(&target_addr);
			// Success ?
			if (!err)
			{
				// Go to connecting central state
				//! SCANNING --> SET_CONNECTING: scanned_target
				app_state_set(APP_STATE_SET_CONNECTING);
			}
			break;

		default:
			printk("  {UNEXPECTED_EVENT}\n");
			break;
		}
	}
	// Invalid event ?
	else
	{
		printk("app_event_scanning(%u) {UNKNOWN}\n", app_evt);
	}

	return err;
}

// App event for connecting state
static int app_event_set_connecting(app_event_t app_evt)
{
	int err = 0;

	// Valid event ?
	if (app_evt < APP_EVENT_MAX)
	{
		printk("app_event_set_connecting(%s)\n", app_event_strings[app_evt]);
		// Which event ?
		switch (app_evt)
		{

		case APP_EVENT_CONNECTED_FAIL:
			// Attempt scan
			err = app_scan_start();
			// Success ?
			if (!err)
			{
				// Go to scanning state
				//! SET_CONNECTING --> SCANNING: connected_fail
				app_state_set(APP_STATE_SCANNING);
			}
			break;

		case APP_EVENT_CONNECTED_CENTRAL:
			// Attempt service discovery
			err = app_discover_service(&bt_uuid_ias.uuid);
			// Success ?
			if (!err)
			{
				// Go to discovering ias state
				//! SET_CONNECTING --> DISCOVERING_IAS: connected_central
				app_state_set(APP_STATE_DISCOVERING_IAS);
			}
			// Fail, clear error and disconnect
			break;

		default:
			printk("  {UNEXPECTED_EVENT}\n");
			break;
		}
	}
	// Invalid event ?
	else
	{
		printk("app_event_set_connecting(%u) {UNKNOWN}\n", app_evt);
	}

	return err;
}

// App event for discovering IAS state
static int app_event_discovering_ias(app_event_t app_evt)
{
	int err = 0;

	// Valid event ?
	if (app_evt < APP_EVENT_MAX)
	{
		printk("app_event_discovering_ias(%s)\n", app_event_strings[app_evt]);
		// Which event ?
		switch (app_evt)
		{

		case APP_EVENT_DISCOVER_SERVICE_FAIL:
			// Connected ?
			if (bt_current_conn)
			{
				// Attempt disconnection
				err = app_disconnect();
				// Success ?
				if (!err)
				{
					// Go to disconnecting central state
					//! DISCOVERING_IAS --> SET_DISCONNECTING: discover_service_fail<br>(connected)
					app_state_set(APP_STATE_SET_DISCONNECTING);
				}
			}
			// Not connected ?
			else
			{
				// Start scanning
				err = app_scan_start();
				// Success ?
				if (!err)
				{
					// Assume it failed due to connection being lost
					// so remove from ignore list so we can try again
					target_ignore_stack_pop();
					// Go to scanning state
					//! DISCOVERING_IAS --> SCANNING: discover_service_fail<br>(not connected)
					app_state_set(APP_STATE_SCANNING);
				}
			}
			break;

		case APP_EVENT_DISCOVER_SERVICE_OK:
			// Attempt characteristic discovery
			err = app_discover_characteristic(&bt_uuid_alc.uuid);
			// Success ?
			if (!err)
			{
				// Go to discovering alc state
				//! DISCOVERING_IAS --> DISCOVERING_ALC: discover_service_ok
				app_state_set(APP_STATE_DISCOVERING_ALC);
			}
			// Fail, clear error and disconnect
			break;

		case APP_EVENT_DISCONNECTED_CENTRAL:
			// Start scanning
			err = app_scan_start();
			// Success ?
			if (!err)
			{
				// Go to scanning state
				//! DISCOVERING_IAS --> SCANNING: disconnected_central
				app_state_set(APP_STATE_SCANNING);
			}
			break;

		default:
			printk("  {UNEXPECTED_EVENT}\n");
			break;
		}
	}
	// Invalid event ?
	else
	{
		printk("app_event_discovering_ias(%u) {UNKNOWN}\n", app_evt);
	}

	return err;
}

// App event for discovering ALC state
static int app_event_discovering_alc(app_event_t app_evt)
{
	int err = 0;

	// Valid event ?
	if (app_evt < APP_EVENT_MAX)
	{
		printk("app_event_discovering_alc(%s)\n", app_event_strings[app_evt]);
		// Which event ?
		switch (app_evt)
		{

		case APP_EVENT_DISCOVER_CHARACTERISTIC_FAIL:
			// Connected ?
			if (bt_current_conn)
			{
				// Attempt disconnection
				err = app_disconnect();
				// Success ?
				if (!err)
				{
					// Go to disconnecting central state
					//! DISCOVERING_ALC --> SET_DISCONNECTING: discover_characteristic_fail<br>(connected)
					app_state_set(APP_STATE_SET_DISCONNECTING);
				}
			}
			// Not connected ?
			else
			{
				// Start scanning
				err = app_scan_start();
				// Success ?
				if (!err)
				{
					// Assume it failed due to connection being lost
					// so remove from ignore list so we can try again
					target_ignore_stack_pop();
					// Go to scanning state
					//! DISCOVERING_ALC --> SCANNING: discover_characteristic_fail<br>(not connected)
					app_state_set(APP_STATE_SCANNING);
				}
			}
			break;

		case APP_EVENT_DISCOVER_CHARACTERISTIC_OK:
			// Attempt to write alert level
			err = app_write_alert_level(app_alert_level - APP_ALERT_LEVEL_LOCATOR_NONE);
			// Success ?
			if (!err)
			{
				// Go to writing state
				//! DISCOVERING_ALC --> SET_WRITING_ALC: discover_characteristic_ok
				app_state_set(APP_STATE_SET_WRITING_ALC);
				// Schedule timeout
				k_work_schedule(&bt_timeout_work, K_MSEC(TIMEOUT_MS));
			}
			// Fail, clear error and disconnect
			break;

		case APP_EVENT_DISCONNECTED_CENTRAL:
			// Start scanning
			err = app_scan_start();
			// Success ?
			if (!err)
			{
				// Go to scanning state
				//! DISCOVERING_ALC --> SCANNING: disconnected_central
				app_state_set(APP_STATE_SCANNING);
			}
			break;

		default:
			printk("  {UNEXPECTED_EVENT}\n");
			break;
		}
	}
	// Invalid event ?
	else
	{
		printk("app_event_discovering_alc(%u) {UNKNOWN}\n", app_evt);
	}

	return err;
}

// App event for writing ALC state
static int app_event_set_writing_alc(app_event_t app_evt)
{
	int err = 0;

	// Valid event ?
	if (app_evt < APP_EVENT_MAX)
	{
		printk("app_event_set_writing_alc(%s)\n", app_event_strings[app_evt]);
		// Which event ?
		switch (app_evt)
		{

		case APP_EVENT_TIMEOUT:
			// Attempt disconnection
			err = app_disconnect();
			// Success ?
			if (!err)
			{
				// Go to disconnecting central state
				//! SET_WRITING_ALC --> SET_DISCONNECTING: timeout
				app_state_set(APP_STATE_SET_DISCONNECTING);
			}
			break;

		case APP_EVENT_DISCONNECTED_CENTRAL:
			// Cancel timeout
			k_work_cancel_delayable(&bt_timeout_work);
			// Start scanning
			err = app_scan_start();
			// Success ?
			if (!err)
			{
				// Go to scanning state
				//! SET_WRITING_ALC --> SCANNING: disconnected_central
				app_state_set(APP_STATE_SCANNING);
			}
			break;

		default:
			printk("  {UNEXPECTED_EVENT}\n");
			break;
		}
	}
	// Invalid event ?
	else
	{
		printk("app_event_set_writing_alc(%u) {UNKNOWN}\n", app_evt);
	}

	return err;
}

// App event for disconnecting state
static int app_event_set_disconnecting(app_event_t app_evt)
{
	int err = 0;

	// Valid event ?
	if (app_evt < APP_EVENT_MAX)
	{
		printk("app_event_set_disconnecting(%s)\n", app_event_strings[app_evt]);
		// Which event ?
		switch (app_evt)
		{
		case APP_EVENT_DISCONNECTED_CENTRAL:
			// Start scanning
			err = app_scan_start();
			// Success ?
			if (!err)
			{
				// Go to scanning state
				//! SET_DISCONNECTING --> SCANNING: disconnected_central
				app_state_set(APP_STATE_SCANNING);
			}
			break;
		default:
			printk("  {UNEXPECTED_EVENT}\n");
			break;
		}
	}
	// Invalid event ?
	else
	{
		printk("app_event_set_disconnecting(%u) {UNKNOWN}\n", app_evt);
	}

	return err;
}

// App event dispatcher
static void app_event(app_event_t app_evt)
{
	int err = 0;
	bool app_event_consumed = false;

	// Valid event ?
	if (app_evt < APP_EVENT_MAX)
	{
		printk("app_event(%s)\n", app_event_strings[app_evt]);
		// Events that might be processed in any state ?
		if (app_evt == APP_EVENT_PRESSED_SW0 ||
			app_evt == APP_EVENT_PRESSED_SW1)
		{
			// In target alert ?
			if (app_alert_level == APP_ALERT_LEVEL_TARGET_MILD ||
				app_alert_level == APP_ALERT_LEVEL_TARGET_HIGH)
			{
				// Cancel alert
				app_alert_level_set(APP_ALERT_LEVEL_TARGET_NONE);
				// We have consumed the event
				app_event_consumed = true;
			}
		}
		// Didn't consume event already ?
		if (!app_event_consumed)
		{
			// Valid state ?
			if (app_state < APP_STATE_MAX)
			{
				// What state are we in ?
				switch (app_state)
				{
				case APP_STATE_IDLE:
					err = app_event_idle(app_evt);
					break;
				case APP_STATE_ADVERTISING:
					err = app_event_advertising(app_evt);
					break;
				case APP_STATE_CONNECTED:
					err = app_event_connected(app_evt);
					break;
				case APP_STATE_SCANNING:
					err = app_event_scanning(app_evt);
					break;
				case APP_STATE_SET_CONNECTING:
					err = app_event_set_connecting(app_evt);
					break;
				case APP_STATE_DISCOVERING_IAS:
					err = app_event_discovering_ias(app_evt);
					break;
				case APP_STATE_DISCOVERING_ALC:
					err = app_event_discovering_alc(app_evt);
					break;
				case APP_STATE_SET_WRITING_ALC:
					err = app_event_set_writing_alc(app_evt);
					break;
				case APP_STATE_SET_DISCONNECTING:
					err = app_event_set_disconnecting(app_evt);
					break;
				default:
					printk("  {UNEXPECTED_STATE}\n");
					break;
				}
			}
			// Invalid state ?
			else
			{
				printk("  app_state == %u {INVALID}\n", app_evt);
			}
		}

		// Something went wrong ?
		if (err)
		{
			// Go to idle state
			//! any --> IDLE: error
			app_state_set(APP_STATE_IDLE);
		}
	}
	// Invalid event ?
	else
	{
		printk("app_event(%u) {INVALID}\n", app_evt);
	}
}

// App work handler
static void app_work_handler(struct k_work *work)
{
	app_event_t evt;

	printk("app_work_handler()\n");
	// Process events
	while (k_msgq_get(&app_event_queue, &evt, K_NO_WAIT) == 0)
	{
		app_event(evt);
	}
}

// Main function
int main(void)
{
	int err;

	// Debug system information
	printk("\nmain()\n");
	printk("  CMAKE_PROJECT_NAME              = %s\n", CMAKE_PROJECT_NAME);
	printk("  CONFIG_BT_DEVICE_NAME           = %s\n", CONFIG_BT_DEVICE_NAME);
	printk("  CONFIG_BT_DIS_MODEL_NUMBER_STR  = %s\n", CONFIG_BT_DIS_MODEL_NUMBER_STR);
	printk("  CONFIG_BT_DIS_MANUF_NAME_STR    = %s\n", CONFIG_BT_DIS_MANUF_NAME_STR);
	printk("  CONFIG_BT_DIS_SERIAL_NUMBER_STR = %s\n", CONFIG_BT_DIS_SERIAL_NUMBER_STR);
	printk("  CONFIG_BT_DIS_FW_REV_STR        = %s\n", CONFIG_BT_DIS_FW_REV_STR);
	printk("  CONFIG_BT_DIS_HW_REV_STR        = %s\n", CONFIG_BT_DIS_HW_REV_STR);
	printk("  CONFIG_BT_DIS_SW_REV_STR        = %s\n", CONFIG_BT_DIS_SW_REV_STR);

	// Enable BLE
	err = bt_enable(NULL);
	printk("  bt_enable() = %d = %s\n", err, strerror(-err));
	// Success
	if (!err)
	{
		// Register modern scan callback
		bt_le_scan_cb_register(&scan_cb);
		// Get id addresses
		bt_addr_le_t id_addrs[CONFIG_BT_ID_MAX];
		size_t count = ARRAY_SIZE(id_addrs);
		bt_id_get(id_addrs, &count);
		printk("  bt_id_get(%zu)\n", count);
		// Success ?
		if (!err)
		{
			// Output id addresses
			for (size_t i = 0; i < count; i++)
			{
				char addr_str[BT_ADDR_LE_STR_LEN];
				bt_addr_le_to_str(&id_addrs[i], addr_str, sizeof(addr_str));
				printk("  id_addrs[%zu] = %s\n", i, addr_str);
			}
			// Set up write timeout work item
			k_work_init_delayable(&bt_timeout_work, app_timeout_work_handler);
		}

		// Initialise app work
		k_work_init(&app_work, app_work_handler);
		// Start state machine processing
		app_event_queue_add(APP_EVENT_START);

		// Set up output work item
		k_work_init_delayable(&output_work, output_work_handler);
		// Schedule output work item
		k_work_schedule(&output_work, K_NO_WAIT);
	}

	return 0;
}
//! classDef green fill:#d4f8d4,stroke:#2e7d32,color:#000
//! classDef blue fill:#d0e7ff,stroke:#1565c0,color:#000
//! class IDLE,ADVERTISING,CONNECTED,any green
//! class SCANNING,SET_CONNECTING,DISCOVERING_IAS,DISCOVERING_ALC,SET_WRITING_ALC,SET_DISCONNECTING blue
//! ```