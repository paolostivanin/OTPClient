#pragma once

#define UNUSED __attribute__((unused))

#define CAST_USER_DATA(type, var, data) type *var = (type *)(data)