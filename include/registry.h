#ifndef REGISTRY_H
#define REGISTRY_H

#include "driver.h"

const struct driver_ops *registry_lookup(const char *module_type);

#endif
