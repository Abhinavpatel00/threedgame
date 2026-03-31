#include "input.h"

#include <string.h>

#include <GLFW/glfw3.h>

static int keycode_to_glfw(KeyCode key)
{
    switch(key)
    {
        case KEY_SPACE:
            return GLFW_KEY_SPACE;
        case KEY_LEFT:
            return GLFW_KEY_LEFT;
        case KEY_RIGHT:
            return GLFW_KEY_RIGHT;
        case KEY_UP:
            return GLFW_KEY_UP;
        case KEY_DOWN:
            return GLFW_KEY_DOWN;
        case KEY_R:
            return GLFW_KEY_R;
        case KEY_1:
            return GLFW_KEY_1;
        case KEY_2:
            return GLFW_KEY_2;
        default:
            return 0;
    }
}

void input_init(Input* in)
{
    if(!in)
        return;

    memset(in, 0, sizeof(*in));
}

void input_update(Input* in, struct GLFWwindow* window)
{
    if(!in)
        return;

    for(int i = 0; i < (int)KEY_COUNT; ++i)
    {
        KeyState* k = &in->keys[i];
        k->prev = k->curr;

        int glfw_key = keycode_to_glfw((KeyCode)i);
        if(!window || glfw_key == 0)
        {
            k->curr = false;
            continue;
        }

        k->curr = glfwGetKey(window, glfw_key) == GLFW_PRESS;
    }
}

bool input_down(const Input* in, KeyCode key)
{
    if(!in || key >= KEY_COUNT)
        return false;

    return in->keys[key].curr;
}

bool input_pressed(const Input* in, KeyCode key)
{
    if(!in || key >= KEY_COUNT)
        return false;

    KeyState k = in->keys[key];
    return k.curr && !k.prev;
}

bool input_released(const Input* in, KeyCode key)
{
    if(!in || key >= KEY_COUNT)
        return false;

    KeyState k = in->keys[key];
    return !k.curr && k.prev;
}
