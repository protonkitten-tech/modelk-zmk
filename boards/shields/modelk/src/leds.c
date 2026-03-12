#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/hid_indicators.h>
#include <zmk/keymap.h>
#include <zmk/physical_layouts.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/endpoints_types.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

// GPIO configuration for Num Lock LED
// ** Requires LEDs to be named 'numlock_led' **
static const struct gpio_dt_spec numlock_led = GPIO_DT_SPEC_GET(DT_NODELABEL(numlock_led), gpios);
static const struct gpio_dt_spec capslock_led = GPIO_DT_SPEC_GET(DT_NODELABEL(capslock_led), gpios);
static const struct gpio_dt_spec bluetooth_led = GPIO_DT_SPEC_GET(DT_NODELABEL(bluetooth_led), gpios);

// For faster initialization
static const struct gpio_dt_spec *leds[] = {&numlock_led, &capslock_led, &bluetooth_led};
static const char *led_names[] = {"NumLock", "CapsLock", "Bluetooth"};


// Global variables to store the current state
static bool numlock_active = false;
static bool capslock_active = false;
static bool is_idle = false;
static bool bluetooth_connected = false;
static bool bluetooth_pairing = false;

// Bluetooth LED blinking for pairing mode
static struct k_timer bluetooth_blink_timer;
static bool bluetooth_blink_state = false;

// Helper to set the state of a LED
static int set_led(bool state, struct gpio_dt_spec led) {
	int err = gpio_pin_set_dt(&led, state ? 1 : 0);
	if (err) LOG_ERR("Error setting GPIO pin: %d", err);
	return err;
}

// Function to apply states to LEDs
static void apply_led_state(void) {
	// If idle and the LED should be on, turn it off during idle
	if (is_idle && !bluetooth_pairing) {
		set_led(false, numlock_led);
		set_led(false, capslock_led);
		set_led(false, bluetooth_led);
		return;
	}
	k_timer_stop(&bluetooth_blink_timer);

	if (bluetooth_pairing) {
		// Blinking is handled by timer, just ensure it's started
		if (!k_timer_status_get(&bluetooth_blink_timer)) {
			k_timer_start(&bluetooth_blink_timer, K_MSEC(250), K_MSEC(250));
		}
	} else {
		k_timer_stop(&bluetooth_blink_timer);
		set_led(bluetooth_connected, bluetooth_led);
	}

	set_led(numlock_active, numlock_led);
	set_led(capslock_active, capslock_led);
}

// Update HID indicators status
// Does not apply to LEDs, use apply_led_state() right after
static void update_hid_indicator_flags(void) {
	zmk_hid_indicators_t flags = zmk_hid_indicators_get_current_profile();

	// Update Num Lock state
	numlock_active = (flags & 0x01) ? true : false;
	capslock_active = (flags & 0x02) ? true : false;

	LOG_INF("NUMLOCK: %s, CAPSLOCK: %s",
			numlock_active ? "ON" : "OFF",
			capslock_active ? "ON" : "OFF");
}

// Update Bluetooth connection status
static void update_bluetooth_status(void) {
	struct zmk_endpoint_instance active_endpoint = zmk_endpoints_selected();
	bool ble_is_selected = (active_endpoint.transport == ZMK_TRANSPORT_BLE);
	bool ble_is_connected = false;

	// Check if BLE is actually connected (not just selected)
	if (ble_is_selected) {
		// Check if the active BLE profile is connected
		uint8_t active_profile = zmk_ble_active_profile_index();
		ble_is_connected = zmk_ble_active_profile_is_connected();
	}

	bluetooth_connected = ble_is_connected;

	// Check if we're in pairing mode:
	// - BLE is selected but not connected
	// - OR BLE profile is open (advertising)
	bluetooth_pairing = zmk_ble_active_profile_is_open();

	if (bluetooth_pairing && !bluetooth_connected) {
		// Start blinking timer (500ms interval for 2Hz blink rate)
		k_timer_start(&bluetooth_blink_timer, K_MSEC(500), K_MSEC(500));
	} else {
		k_timer_stop(&bluetooth_blink_timer);
		bluetooth_blink_state = false;
	}

	LOG_INF("Bluetooth - Selected: %s, Connected: %s, Pairing: %s",
			ble_is_selected ? "YES" : "NO",
			ble_is_connected ? "YES" : "NO",
			bluetooth_pairing ? "YES" : "NO");

	// Also update HID because race condition that doesn't make it update
	update_hid_indicator_flags();
}

// Timer callback for Bluetooth LED blinking
static void bluetooth_blink_timer_handler(struct k_timer *timer_id) {
	if (bluetooth_pairing) {
		bluetooth_blink_state = !bluetooth_blink_state;
		set_led(bluetooth_blink_state, bluetooth_led);
	}
}

// Callback when the keyboard enters idle
static void on_idle(void) {
	// printk("Keyboard is idle. Updating Num Lock LED state.\n");
	is_idle = true;
	apply_led_state();
}

// Callback when the keyboard wakes up from idle
static void on_wake_up(void) {
	// printk("Keyboard has woken up from idle. Restoring Num Lock LED state.\n");
	is_idle = false;

	update_bluetooth_status(); // Gets called in update_bluetooth_status
							   // update_hid_indicator_flags();
	apply_led_state();
}

// Event listeners
static int on_activity_state_changed(const zmk_event_t *ev) {
	const struct zmk_activity_state_changed *activity_event = as_zmk_activity_state_changed(ev);
	if (!activity_event) return -1;

	if (activity_event->state == ZMK_ACTIVITY_IDLE) {
		on_idle();
	} else if (activity_event->state == ZMK_ACTIVITY_ACTIVE) {
		on_wake_up();
	}

	return ZMK_EV_EVENT_BUBBLE;
}


static int status_changed_listener_cb(const zmk_event_t *eh) {
	update_bluetooth_status();
	// update_hid_indicator_flags(); // Gets called in update_bluetooth_status
	apply_led_state();
	return ZMK_EV_EVENT_BUBBLE;
}

// Future me: See https://github.com/zmkfirmware/zmk/tree/main/app/include/zmk/events for event types
ZMK_LISTENER(activity_state_changed_listener, on_activity_state_changed);
ZMK_SUBSCRIPTION(activity_state_changed_listener, zmk_activity_state_changed);

ZMK_LISTENER(something_changed_listener, status_changed_listener_cb);
ZMK_SUBSCRIPTION(something_changed_listener, zmk_ble_active_profile_changed);
ZMK_SUBSCRIPTION(something_changed_listener, zmk_hid_indicators_changed);
ZMK_SUBSCRIPTION(something_changed_listener, zmk_endpoint_changed);

// Initialize the LED and work structures on boot
static int leds_init(void) {
	// Check all LEDs are ready and configure them
	for (int i = 0; i < ARRAY_SIZE(leds); i++) {
		if (!gpio_is_ready_dt(leds[i])) {
			LOG_ERR("GPIO device for %s LED not ready", led_names[i]);
			return -ENODEV;
		}

		int err = gpio_pin_configure_dt(leds[i], GPIO_OUTPUT_INACTIVE);
		if (err) {
			LOG_ERR("Error configuring %s LED GPIO: %d", led_names[i], err);
			return err;
		}
	}

	// Initialize Bluetooth blink timer
	k_timer_init(&bluetooth_blink_timer, bluetooth_blink_timer_handler, NULL);

	// Initialize state and apply
	is_idle = false;
	numlock_active = false;
	capslock_active = false;
	bluetooth_connected = false;
	bluetooth_pairing = false;
	update_bluetooth_status();
	// update_hid_indicator_flags(); // Gets called in update_bluetooth_status
	apply_led_state();

	LOG_INF("LEDs initialized successfully");
	return 0;
}

// Register leds_init to run at boot
SYS_INIT(leds_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
