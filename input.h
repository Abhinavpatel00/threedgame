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

typedef struct
{
    bool curr;
    bool prev;
} KeyState;

typedef struct
{
    KeyState keys[KEY_COUNT];
} Input;

void input_init(Input* in);
void input_update(Input* in, struct GLFWwindow* window);

bool input_down(const Input* in, KeyCode key);
bool input_pressed(const Input* in, KeyCode key);
bool input_released(const Input* in, KeyCode key);
