/* main.c - Application main entry point */

/*
 * Copyright (c) 2026 Silicon Labs
 *
 * SPDX-License-Identifier: Apache-2.0
 */

//! **Find Me (Step 2c: Piezo) - State Transition Diagram**
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
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>

#include <zephyr/drivers/led.h>
#include <zephyr/drivers/pwm.h>

#include <zephyr/input/input.h>

#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

// APP DATA -------------------------------------------------------------------

// App states
#define APP_STATE_LIST(X)    \
	X(APP_STATE_IDLE)        \
	X(APP_STATE_ADVERTISING) \
	X(APP_STATE_CONNECTED)

// App events
#define APP_EVENT_LIST(X)             \
	X(APP_EVENT_START)                \
	X(APP_EVENT_PRESSED_SW0)          \
	X(APP_EVENT_PRESSED_SW1)          \
	X(APP_EVENT_CONNECTED_PERIPHERAL) \
	X(APP_EVENT_DISCONNECTED_PERIPHERAL)

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

#undef GENERATE_ENUM

// Create state and evevnt strings
#define GENERATE_STRING(name) #name,

static const char *app_state_strings[APP_STATE_MAX] = {
	APP_STATE_LIST(GENERATE_STRING)};

static const char *app_event_strings[APP_EVENT_MAX] = {
	APP_EVENT_LIST(GENERATE_STRING)};

#undef GENERATE_STRING

// App state
static app_state_t app_state = APP_STATE_IDLE;

// App event message queue
#define APP_EVENT_QUEUE_DEPTH 8
K_MSGQ_DEFINE(app_event_queue, sizeof(app_event_t), APP_EVENT_QUEUE_DEPTH, 4);

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
	{true, true, true, 0, 50, 1450},				  // APP_STATE_IDLE
	{false, false, true, 0, 50, 1450},				  // APP_STATE_ADVERTISING
	{false, true, false, NOTE_C4_PERIOD_NS, 50, 1450} // APP_STATE_CONNECTED
};
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
	BT_DATA_BYTES(BT_DATA_UUID16_ALL, BT_UUID_16_ENCODE(BT_UUID_DIS_VAL)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

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
	output_config_index = app_state;
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

// APP BLE HELPER CODE --------------------------------------------------------

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
			}
		}
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
		}
	}
}

// Define BLE connection state callbacks
BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = app_connected_cb,
	.disconnected = app_disconnected_cb,
};

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

// App event dispatcher
static void app_event(app_event_t app_evt)
{
	int err = 0;
	bool app_event_consumed = false;

	// Valid event ?
	if (app_evt < APP_EVENT_MAX)
	{
		printk("app_event(%s)\n", app_event_strings[app_evt]);
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
//! class IDLE,ADVERTISING,CONNECTED,any green
//! ```