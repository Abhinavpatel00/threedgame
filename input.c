#include "input.h"

#include <math.h>
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

static int mouse_button_to_glfw(MouseButton button)
{
    switch(button)
    {
        case MOUSE_LEFT:
            return GLFW_MOUSE_BUTTON_LEFT;
        case MOUSE_RIGHT:
            return GLFW_MOUSE_BUTTON_RIGHT;
        case MOUSE_MIDDLE:
            return GLFW_MOUSE_BUTTON_MIDDLE;
        default:
            return -1;
    }
}

static void input_scroll_callback(GLFWwindow* window, double xoffset, double yoffset)
{
    Input* in = (Input*)glfwGetWindowUserPointer(window);
    if(!in)
        return;

    input_on_scroll(in, xoffset, yoffset);
}

void input_init(Input* in)
{
    if(!in)
        return;

    memset(in, 0, sizeof(*in));
}

void input_attach(Input* in, GLFWwindow* window)
{
    if(!in || !window)
        return;

    glfwSetWindowUserPointer(window, in);
    glfwSetScrollCallback(window, input_scroll_callback);
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

    for(int i = 0; i < (int)MOUSE_BUTTON_COUNT; ++i)
    {
        KeyState* k = &in->mouse.buttons[i];
        k->prev = k->curr;

        int glfw_button = mouse_button_to_glfw((MouseButton)i);
        if(!window || glfw_button < 0)
        {
            k->curr = false;
            continue;
        }

        k->curr = glfwGetMouseButton(window, glfw_button) == GLFW_PRESS;
    }

    double mouse_x = 0.0;
    double mouse_y = 0.0;
    if(window)
        glfwGetCursorPos(window, &mouse_x, &mouse_y);

    if(!in->mouse_initialized)
    {
        in->last_mouse_x = mouse_x;
        in->last_mouse_y = mouse_y;
        in->mouse_initialized = true;
        in->mouse.dx = 0.0;
        in->mouse.dy = 0.0;
    }
    else
    {
        in->mouse.dx = mouse_x - in->last_mouse_x;
        in->mouse.dy = mouse_y - in->last_mouse_y;
        in->last_mouse_x = mouse_x;
        in->last_mouse_y = mouse_y;
    }

    in->mouse.x = mouse_x;
    in->mouse.y = mouse_y;

    in->mouse.scroll_x = in->pending_scroll_x;
    in->mouse.scroll_y = in->pending_scroll_y;
    in->pending_scroll_x = 0.0;
    in->pending_scroll_y = 0.0;
}

void input_on_scroll(Input* in, double xoffset, double yoffset)
{
    if(!in)
        return;

    in->pending_scroll_x += xoffset;
    in->pending_scroll_y += yoffset;
}

void input_actions_default(ActionMap* map)
{
    if(!map)
        return;

    memset(map, 0, sizeof(*map));
    for(int i = 0; i < ACTION_COUNT; ++i)
        map->bindings[i].key = KEY_COUNT;

    input_action_bind(map, ACTION_JUMP, KEY_SPACE);
    input_action_bind(map, ACTION_CYCLE_ANIM_NEXT, KEY_RIGHT);
    input_action_bind(map, ACTION_CYCLE_ANIM_PREV, KEY_LEFT);
    input_action_bind(map, ACTION_RESET_ANIM, KEY_R);
    input_action_bind(map, ACTION_SPEED_UP, KEY_UP);
    input_action_bind(map, ACTION_SPEED_DOWN, KEY_DOWN);
    input_action_bind(map, ACTION_LOAD_MODEL_1, KEY_1);
    input_action_bind(map, ACTION_LOAD_MODEL_2, KEY_2);
    input_action_bind(map, ACTION_TOGGLE_PAUSE, KEY_SPACE);
}

void input_action_bind(ActionMap* map, ActionCode action, KeyCode key)
{
    if(!map || action >= ACTION_COUNT || key >= KEY_COUNT)
        return;

    map->bindings[action].key = key;
}

bool input_action_down(const Input* in, const ActionMap* map, ActionCode action)
{
    if(!in || !map || action >= ACTION_COUNT)
        return false;

    KeyCode key = map->bindings[action].key;
    if(key >= KEY_COUNT)
        return false;

    return input_down(in, key);
}

bool input_action_pressed(const Input* in, const ActionMap* map, ActionCode action)
{
    if(!in || !map || action >= ACTION_COUNT)
        return false;

    KeyCode key = map->bindings[action].key;
    if(key >= KEY_COUNT)
        return false;

    return input_pressed(in, key);
}

bool input_action_released(const Input* in, const ActionMap* map, ActionCode action)
{
    if(!in || !map || action >= ACTION_COUNT)
        return false;

    KeyCode key = map->bindings[action].key;
    if(key >= KEY_COUNT)
        return false;

    return input_released(in, key);
}

void input_get_move_vector(const Input* in, KeyCode left, KeyCode right, KeyCode up, KeyCode down, float* out_x,
                           float* out_z)
{
    if(!out_x || !out_z)
        return;

    float move_x = 0.0f;
    float move_z = 0.0f;

    if(in)
    {
        if(input_down(in, left))
            move_x -= 1.0f;
        if(input_down(in, right))
            move_x += 1.0f;
        if(input_down(in, up))
            move_z += 1.0f;
        if(input_down(in, down))
            move_z -= 1.0f;
    }

    float len = sqrtf(move_x * move_x + move_z * move_z);
    if(len > 0.0f)
    {
        move_x /= len;
        move_z /= len;
    }

    *out_x = move_x;
    *out_z = move_z;
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
