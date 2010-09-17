/* -*- Mode: C; c-basic-offset:4 ; -*- */
/*
 *  (C) 2008 by Argonne National Laboratory.
 *      See COPYRIGHT in top-level directory.
 */

#include "hydra_utils.h"
#include "bind.h"
#include "bind_hwloc.h"

static hwloc_topology_t topology;

#define dprint(d, ...)                           \
    do {                                         \
        int _i;                                  \
        for (_i = 0; _i < d; _i++)               \
            fprintf(stderr, "    ");             \
        fprintf(stderr, __VA_ARGS__);            \
    } while (0)

/* Retained for debugging purposes */
static void print_obj_info(hwloc_obj_t obj) ATTRIBUTE((unused));
static void print_obj_info(hwloc_obj_t obj)
{
    int i;

    if (obj->type == HWLOC_OBJ_CACHE)
        dprint(obj->depth, "[%s] L%u cache size: %lu\n",
               hwloc_obj_type_string(obj->type), obj->attr->cache.depth,
               obj->attr->cache.size);
    else {
        if (obj->memory.total_memory || obj->memory.local_memory)
            dprint(obj->depth, "[%s:%u] total memory: %lu; local memory: %lu\n",
                   hwloc_obj_type_string(obj->type), obj->os_index, obj->memory.total_memory,
                   obj->memory.local_memory);
        else
            dprint(obj->depth, "[%s:%u]\n", hwloc_obj_type_string(obj->type), obj->os_index);
    }

    for (i = 0; i < obj->arity; i++)
        print_obj_info(obj->children[i]);
}

static HYD_status load_mem_cache_info(struct HYDT_topo_obj *obj, hwloc_obj_t hobj)
{
    int total_cache_objs, i, j;
    hwloc_obj_t obj_cache;
    HYD_status status = HYD_SUCCESS;

    obj->mem.local_mem_size = hobj->memory.local_memory;

    /* Check how many cache objects match out cpuset */
    total_cache_objs = hwloc_get_nbobjs_by_type(topology, HWLOC_OBJ_CACHE);
    obj->mem.num_caches = 0;
    for (i = 0; i < total_cache_objs; i++) {
        if (hwloc_get_obj_inside_cpuset_by_type(topology, hobj->cpuset,
                                                HWLOC_OBJ_CACHE, i))
            obj->mem.num_caches++;
    }

    HYDU_MALLOC(obj->mem.cache_size, size_t *, obj->mem.num_caches * sizeof(size_t),
                status);
    HYDU_MALLOC(obj->mem.cache_depth, int *, obj->mem.num_caches * sizeof(int),
                status);

    for (i = 0, j = 0; i < total_cache_objs; i++) {
        obj_cache = hwloc_get_obj_inside_cpuset_by_type(topology, hobj->cpuset,
                                                        HWLOC_OBJ_CACHE, i);
        if (obj_cache) {
            obj->mem.cache_size[j] = obj_cache->attr->cache.size;
            obj->mem.cache_depth[j] = obj_cache->attr->cache.depth;
            j++;
        }
    }

  fn_exit:
    return status;

  fn_fail:
    goto fn_exit;
}

HYD_status HYDT_bind_hwloc_init(HYDT_bind_support_level_t * support_level)
{
    int node, sock, core, thread;

    hwloc_obj_t obj_sys;
    hwloc_obj_t obj_node;
    hwloc_obj_t obj_sock;
    hwloc_obj_t obj_core;
    hwloc_obj_t obj_thread;

    struct HYDT_topo_obj *node_ptr, *sock_ptr, *core_ptr, *thread_ptr;

    HYD_status status = HYD_SUCCESS;

    HYDU_FUNC_ENTER();

    hwloc_topology_init(&topology);
    hwloc_topology_load(topology);

    /* Get the max number of processing elements */
    HYDT_bind_info.total_proc_units = hwloc_get_nbobjs_by_type(topology, HWLOC_OBJ_PU);

    /* We have qualified for basic binding support level */
    *support_level = HYDT_BIND_BASIC;

    /* Setup the machine level */
    obj_sys = hwloc_get_root_obj(topology);

    /* Retained for debugging purposes */
    /* print_obj_info(obj_sys); */

    /* init Hydra structure */
    HYDT_bind_info.machine.type = HYDT_OBJ_MACHINE;
    HYDT_bind_info.machine.os_index = -1;       /* This is a set, not a single unit */
    HYDT_bind_info.machine.parent = NULL;

    status = load_mem_cache_info(&HYDT_bind_info.machine, obj_sys);
    HYDU_ERR_POP(status, "error loading memory/cache info\n");

    /* There is no real node, consider there is one */
    HYDT_bind_info.machine.num_children = hwloc_get_nbobjs_by_type(topology, HWLOC_OBJ_NODE);
    if (!HYDT_bind_info.machine.num_children)
        HYDT_bind_info.machine.num_children = 1;
    HYDU_MALLOC(HYDT_bind_info.machine.children, struct HYDT_topo_obj *,
                sizeof(struct HYDT_topo_obj) * HYDT_bind_info.machine.num_children, status);

    /* Setup the nodes levels */
    for (node = 0; node < HYDT_bind_info.machine.num_children; node++) {
        node_ptr = &HYDT_bind_info.machine.children[node];
        node_ptr->type = HYDT_OBJ_NODE;
        node_ptr->parent = &HYDT_bind_info.machine;

        obj_node = hwloc_get_obj_inside_cpuset_by_type(topology, obj_sys->cpuset,
                                                       HWLOC_OBJ_NODE, node);

        status = load_mem_cache_info(node_ptr, obj_node);
        HYDU_ERR_POP(status, "error loading memory/cache info\n");

        if (!obj_node)
            obj_node = obj_sys;
        node_ptr->os_index = obj_node->os_index;
        node_ptr->num_children =
            hwloc_get_nbobjs_inside_cpuset_by_type(topology, obj_node->cpuset,
                                                   HWLOC_OBJ_SOCKET);
        /* In case there is no socket */
        if (!node_ptr->num_children)
            node_ptr->num_children = 1;

        HYDU_MALLOC(node_ptr->children, struct HYDT_topo_obj *,
                    sizeof(struct HYDT_topo_obj) * node_ptr->num_children, status);

        /* Setup the socket level */
        for (sock = 0; sock < node_ptr->num_children; sock++) {
            sock_ptr = &node_ptr->children[sock];
            sock_ptr->type = HYDT_OBJ_SOCKET;
            sock_ptr->parent = node_ptr;

            obj_sock = hwloc_get_obj_inside_cpuset_by_type(topology, obj_node->cpuset,
                                                           HWLOC_OBJ_SOCKET, sock);

            status = load_mem_cache_info(sock_ptr, obj_sock);
            HYDU_ERR_POP(status, "error loading memory/cache info\n");

            if (!obj_sock)
                obj_sock = obj_node;

            sock_ptr->os_index = obj_sock->os_index;
            sock_ptr->num_children =
                hwloc_get_nbobjs_inside_cpuset_by_type(topology, obj_sock->cpuset,
                                                       HWLOC_OBJ_CORE);

            /* In case there is no core */
            if (!sock_ptr->num_children)
                sock_ptr->num_children = 1;

            HYDU_MALLOC(sock_ptr->children, struct HYDT_topo_obj *,
                        sizeof(struct HYDT_topo_obj) * sock_ptr->num_children, status);

            /* setup the core level */
            for (core = 0; core < sock_ptr->num_children; core++) {
                core_ptr = &sock_ptr->children[core];
                core_ptr->type = HYDT_OBJ_CORE;
                core_ptr->parent = sock_ptr;

                obj_core = hwloc_get_obj_inside_cpuset_by_type(topology,
                                                               obj_sock->cpuset,
                                                               HWLOC_OBJ_CORE, core);

                status = load_mem_cache_info(core_ptr, obj_core);
                HYDU_ERR_POP(status, "error loading memory/cache info\n");

                if (!obj_core)
                    obj_core = obj_sock;

                core_ptr->os_index = obj_core->os_index;
                core_ptr->num_children =
                    hwloc_get_nbobjs_inside_cpuset_by_type(topology, obj_core->cpuset,
                                                           HWLOC_OBJ_PU);

                /* In case there is no thread */
                if (!core_ptr->num_children)
                    core_ptr->num_children = 1;

                HYDU_MALLOC(core_ptr->children, struct HYDT_topo_obj *,
                            sizeof(struct HYDT_topo_obj) * core_ptr->num_children, status);

                /* setup the thread level */
                for (thread = 0; thread < core_ptr->num_children; thread++) {
                    obj_thread = hwloc_get_obj_inside_cpuset_by_type(topology,
                                                                     obj_core->cpuset,
                                                                     HWLOC_OBJ_PU, thread);
                    thread_ptr = &core_ptr->children[thread];
                    thread_ptr->type = HYDT_OBJ_THREAD;
                    thread_ptr->os_index = obj_thread->os_index;
                    thread_ptr->parent = core_ptr;
                    thread_ptr->num_children = 0;
                    thread_ptr->children = NULL;

                    status = load_mem_cache_info(thread_ptr, obj_thread);
                    HYDU_ERR_POP(status, "error loading memory/cache info\n");
                }
            }
        }
    }

    /* We have qualified for memory topology-aware binding support level */
    *support_level = HYDT_BIND_MEM;

  fn_exit:
    HYDU_FUNC_EXIT();
    return status;

  fn_fail:
    goto fn_exit;
}

HYD_status HYDT_bind_hwloc_process(int os_index)
{
    hwloc_cpuset_t cpuset = hwloc_cpuset_alloc();

    HYD_status status = HYD_SUCCESS;

    HYDU_FUNC_ENTER();

    hwloc_cpuset_set(cpuset, os_index);
    hwloc_set_cpubind(topology, cpuset, HWLOC_CPUBIND_THREAD);

  fn_exit:
    hwloc_cpuset_free(cpuset);
    HYDU_FUNC_EXIT();
    return status;

  fn_fail:
    goto fn_exit;
}
