#pragma once

#include <stdbool.h>

struct GLFWwindow;

typedef enum
{
    KEY_SPACE,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_UP,
    KEY_DOWN,
    KEY_R,
    KEY_1,
    KEY_2,

    KEY_COUNT
} KeyCode;

typedef enum
{
    MOUSE_LEFT,
    MOUSE_RIGHT,
    MOUSE_MIDDLE,

    MOUSE_BUTTON_COUNT
} MouseButton;

typedef enum
{
    ACTION_JUMP,
    ACTION_CYCLE_ANIM_NEXT,
    ACTION_CYCLE_ANIM_PREV,
    ACTION_RESET_ANIM,
    ACTION_SPEED_UP,
    ACTION_SPEED_DOWN,
    ACTION_LOAD_MODEL_1,
    ACTION_LOAD_MODEL_2,
    ACTION_TOGGLE_PAUSE,

    ACTION_COUNT
} ActionCode;

typedef struct
{
    bool curr;
    bool prev;
} KeyState;

typedef struct
{
    KeyState buttons[MOUSE_BUTTON_COUNT];
    double   x;
    double   y;
    double   dx;
    double   dy;
    double   scroll_x;
    double   scroll_y;
} MouseState;

typedef struct
{
    KeyCode key;
} ActionBinding;

typedef struct
{
    ActionBinding bindings[ACTION_COUNT];
} ActionMap;

typedef struct
{
    KeyState keys[KEY_COUNT];
    MouseState mouse;
    double    last_mouse_x;
    double    last_mouse_y;
    double    pending_scroll_x;
    double    pending_scroll_y;
    bool      mouse_initialized;
} Input;

void input_init(Input* in);
void input_attach(Input* in, struct GLFWwindow* window);
void input_update(Input* in, struct GLFWwindow* window);

void input_on_scroll(Input* in, double xoffset, double yoffset);

void input_actions_default(ActionMap* map);
void input_action_bind(ActionMap* map, ActionCode action, KeyCode key);

bool input_action_down(const Input* in, const ActionMap* map, ActionCode action);
bool input_action_pressed(const Input* in, const ActionMap* map, ActionCode action);
bool input_action_released(const Input* in, const ActionMap* map, ActionCode action);

void input_get_move_vector(const Input* in, KeyCode left, KeyCode right, KeyCode up, KeyCode down, float* out_x,
                           float* out_z);

bool input_down(const Input* in, KeyCode key);
bool input_pressed(const Input* in, KeyCode key);
bool input_released(const Input* in, KeyCode key);
