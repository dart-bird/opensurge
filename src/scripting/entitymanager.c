/*
 * Open Surge Engine
 * entitymanager.c - scripting system: Entity Manager
 * Copyright 2008-2024 Alexandre Martins <alemartf(at)gmail.com>
 * http://opensurge2d.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * A SurgeScript object is considered to be an entity if it meets all of the
 * following conditions:
 *
 * 1. the object is tagged "entity"
 * 2. the object is a direct child of Level or a direct child of another entity
 *
 * Objects that are direct children of entities but that are not tagged "entity"
 * are considered to be components. Components are meant to modify the entities
 * in some way.
 *
 * Components may not have any entities as descendants. For example: a child of
 * a component is not considered to be an entity, even if it's tagged "entity".
 *
 * Level setup objects and player companion objects are special cases. They are
 * always considered to be entities, regardless if they are tagged "entity" or
 * not, for backwards compatibility purposes.
 */

#define FASTHASH_INLINE
#include "../util/fasthash.h"

#include <surgescript.h>
#include <string.h>
#include "scripting.h"
#include "../core/logfile.h"
#include "../core/video.h"
#include "../util/v2d.h"
#include "../util/darray.h"
#include "../util/util.h"
#include "../util/stringutil.h"
#include "../util/iterator.h"
#include "../scenes/level.h"

typedef struct entityinfo_t entityinfo_t;
struct entityinfo_t {
    surgescript_objecthandle_t handle; /* hash key: SurgeScript object */
    uint64_t id; /* uniquely identifies the entity in the Level */
    v2d_t spawn_point; /* spawn point */
    bool is_persistent; /* usually placed via level editor; will be saved in the .lev file */
    bool is_sleeping; /* sleeping / inactive? */
};

typedef struct entitydb_t entitydb_t;
struct entitydb_t {

    /* region of interest in world space */
    struct {
        int left, top, right, bottom;
    } roi;

    /* entity info */
    fasthash_t* info;
    fasthash_t* id_to_handle;
    entityinfo_t* cached_query;

    /* late update queue */
    DARRAY(surgescript_objecthandle_t, late_update_queue);

    /* brick-like objects */
    DARRAY(surgescript_objecthandle_t, bricklike_objects);

    /* space partitioning flag */
    bool dirty_partition;

};

static entityinfo_t NULL_ENTRY = { .handle = 0, .id = 0 };
static entityinfo_t* entityinfo_ctor(entityinfo_t info) { return (entityinfo_t*)memcpy(mallocx(sizeof(info)), &info, sizeof(info)); }
static void entityinfo_dtor(void* entry) { free(entry); }

static surgescript_objecthandle_t* handle_ctor(surgescript_objecthandle_t handle) { return (surgescript_objecthandle_t*)memcpy(mallocx(sizeof(handle)), &handle, sizeof(handle)); }
static void handle_dtor(void* handle) { free(handle); }

/* C API; make sure you call these with an actual EntityManager object (it won't be checked) */
bool entitymanager_has_entity_info(surgescript_object_t* entity_manager, surgescript_objecthandle_t entity_handle);
void entitymanager_remove_entity_info(surgescript_object_t* entity_manager, surgescript_objecthandle_t entity_handle);
surgescript_objecthandle_t entitymanager_find_entity_by_id(surgescript_object_t* entity_manager, uint64_t entity_id);
uint64_t entitymanager_get_entity_id(surgescript_object_t* entity_manager, surgescript_objecthandle_t entity_handle);
void entitymanager_set_entity_id(surgescript_object_t* entity_manager, surgescript_objecthandle_t entity_handle, uint64_t entity_id);
v2d_t entitymanager_get_entity_spawn_point(surgescript_object_t* entity_manager, surgescript_objecthandle_t entity_handle);
bool entitymanager_is_entity_persistent(surgescript_object_t* entity_manager, surgescript_objecthandle_t entity_handle);
void entitymanager_set_entity_persistent(surgescript_object_t* entity_manager, surgescript_objecthandle_t entity_handle, bool is_persistent);
bool entitymanager_is_entity_sleeping(surgescript_object_t* entity_manager, surgescript_objecthandle_t entity_handle);
void entitymanager_set_entity_sleeping(surgescript_object_t* entity_manager, surgescript_objecthandle_t entity_handle, bool is_sleeping);
bool entitymanager_is_inside_roi(surgescript_object_t* entity_manager, v2d_t position);
void entitymanager_get_roi(surgescript_object_t* entity_manager, int* top, int* left, int* bottom, int* right);
arrayiterator_t* entitymanager_bricklike_iterator(surgescript_object_t* entity_manager);
ssarrayiterator_t* entitymanager_activeentities_iterator(surgescript_object_t* entity_manager);

/* SurgeScript API */
static surgescript_var_t* fun_main(surgescript_object_t* object, const surgescript_var_t** param, int num_params);
static surgescript_var_t* fun_render(surgescript_object_t* object, const surgescript_var_t** param, int num_params);
static surgescript_var_t* fun_lateupdate(surgescript_object_t* object, const surgescript_var_t** param, int num_params);
static surgescript_var_t* fun_constructor(surgescript_object_t* object, const surgescript_var_t** param, int num_params);
static surgescript_var_t* fun_destructor(surgescript_object_t* object, const surgescript_var_t** param, int num_params);
static surgescript_var_t* fun_destroy(surgescript_object_t* object, const surgescript_var_t** param, int num_params);
static surgescript_var_t* fun_spawn(surgescript_object_t* object, const surgescript_var_t** param, int num_params);
static surgescript_var_t* fun_spawnentity(surgescript_object_t* object, const surgescript_var_t** param, int num_params);
static surgescript_var_t* fun_entity(surgescript_object_t* object, const surgescript_var_t** param, int num_params);
static surgescript_var_t* fun_entityid(surgescript_object_t* object, const surgescript_var_t** param, int num_params);
static surgescript_var_t* fun_findentity(surgescript_object_t* object, const surgescript_var_t** param, int num_params);
static surgescript_var_t* fun_findentities(surgescript_object_t* object, const surgescript_var_t** param, int num_params);
static surgescript_var_t* fun_activeentities(surgescript_object_t* object, const surgescript_var_t** param, int num_params);
static surgescript_var_t* fun_setroi(surgescript_object_t* object, const surgescript_var_t** param, int num_params);
static surgescript_var_t* fun_addtolateupdatequeue(surgescript_object_t* object, const surgescript_var_t** param, int num_params);
static surgescript_var_t* fun_addbricklikeobject(surgescript_object_t* object, const surgescript_var_t** param, int num_params);
static surgescript_var_t* fun_notifyentities(surgescript_object_t* object, const surgescript_var_t** param, int num_params);
static surgescript_var_t* fun_isindebugmode(surgescript_object_t* object, const surgescript_var_t** param, int num_params);
static surgescript_var_t* fun_enterdebugmode(surgescript_object_t* object, const surgescript_var_t** param, int num_params);
static surgescript_var_t* fun_exitdebugmode(surgescript_object_t* object, const surgescript_var_t** param, int num_params);
static surgescript_var_t* fun_getdebugmode(surgescript_object_t* object, const surgescript_var_t** param, int num_params);
static surgescript_var_t* fun_pausecontainers(surgescript_object_t* object, const surgescript_var_t** param, int num_params);
static surgescript_var_t* fun_resumecontainers(surgescript_object_t* object, const surgescript_var_t** param, int num_params);
static surgescript_var_t* fun_getlevel(surgescript_object_t* object, const surgescript_var_t** param, int num_params);
static const surgescript_heapptr_t AWAKEENTITYCONTAINER_ADDR = 0;
static const surgescript_heapptr_t UNAWAKEENTITYCONTAINER_ADDR = 1;
static const surgescript_heapptr_t DEBUGENTITYCONTAINER_ADDR = 2;
static const surgescript_heapptr_t ENTITYTREE_ADDR = 3;
static const surgescript_heapptr_t UNAWAKEENTITYCONTAINERARRAY_ADDR = 4;
static const surgescript_heapptr_t NOTGARBAGECONTAINER_ADDR = 5;

/* helpers */
#define WANT_SPACE_PARTITIONING         1 /* whether or not to optimize unawake entities with space partitioning */
#define generate_entity_id()            (random64() & UINT64_C(0xFFFFFFFF)) /* in Open Surge 0.6.0.x and 0.5.x, we used all 64 bits */
#define get_db(entity_manager)          ((entitydb_t*)surgescript_object_userdata(entity_manager))
#define get_info(db, entity_handle)     ((entityinfo_t*)fasthash_get((db)->info, (entity_handle)))
static inline entityinfo_t* quick_lookup(surgescript_object_t* entity_manager, surgescript_objecthandle_t entity_handle);
static void foreach_unawake_container_inside_roi(surgescript_object_t* entity_manager, const char* fun_name, const surgescript_var_t** param, int num_params);
static void foreach_unawake_container(surgescript_object_t* entity_manager, const char* fun_name, const surgescript_var_t** param, int num_params);
static void foreach_unawake_container_callback(surgescript_objecthandle_t container_handle, void* data);
static void pause_containers(surgescript_object_t* entity_manager, bool pause);
static bool is_in_debug_mode(surgescript_object_t* entity_manager);
static void refresh_entity_tree(surgescript_object_t* entity_manager);
static bool inspect_subtree(const surgescript_object_t* root, bool is_root_entity, const surgescript_objectmanager_t* manager, surgescript_tagsystem_t* tag_system, int depth);
static void prevent_garbage_collection(surgescript_object_t* entity_manager, surgescript_objecthandle_t entity_handle);



/*
 * scripting_register_entitymanager()
 * Register the EntityManager object
 */
void scripting_register_entitymanager(surgescript_vm_t* vm)
{
    surgescript_vm_bind(vm, "EntityManager", "constructor", fun_constructor, 0);
    surgescript_vm_bind(vm, "EntityManager", "destructor", fun_destructor, 0);
    surgescript_vm_bind(vm, "EntityManager", "destroy", fun_destroy, 0);

    surgescript_vm_bind(vm, "EntityManager", "state:main", fun_main, 0);
    surgescript_vm_bind(vm, "EntityManager", "render", fun_render, 0);
    surgescript_vm_bind(vm, "EntityManager", "lateUpdate", fun_lateupdate, 0);
    surgescript_vm_bind(vm, "EntityManager", "addToLateUpdateQueue", fun_addtolateupdatequeue, 1);
    surgescript_vm_bind(vm, "EntityManager", "addBricklikeObject", fun_addbricklikeobject, 1);
    surgescript_vm_bind(vm, "EntityManager", "setROI", fun_setroi, 4);

    surgescript_vm_bind(vm, "EntityManager", "spawn", fun_spawn, 1);
    surgescript_vm_bind(vm, "EntityManager", "spawnEntity", fun_spawnentity, 2);
    surgescript_vm_bind(vm, "EntityManager", "entity", fun_entity, 1);
    surgescript_vm_bind(vm, "EntityManager", "entityId", fun_entityid, 1);
    surgescript_vm_bind(vm, "EntityManager", "findEntity", fun_findentity, 1);
    surgescript_vm_bind(vm, "EntityManager", "findEntities", fun_findentities, 1);
    surgescript_vm_bind(vm, "EntityManager", "activeEntities", fun_activeentities, 0);
    surgescript_vm_bind(vm, "EntityManager", "notifyEntities", fun_notifyentities, 1);

    surgescript_vm_bind(vm, "EntityManager", "isInDebugMode", fun_isindebugmode, 0);
    surgescript_vm_bind(vm, "EntityManager", "enterDebugMode", fun_enterdebugmode, 0);
    surgescript_vm_bind(vm, "EntityManager", "exitDebugMode", fun_exitdebugmode, 0);
    surgescript_vm_bind(vm, "EntityManager", "get_debugMode", fun_getdebugmode, 0);

    surgescript_vm_bind(vm, "EntityManager", "pauseContainers", fun_pausecontainers, 0);
    surgescript_vm_bind(vm, "EntityManager", "resumeContainers", fun_resumecontainers, 0);

    surgescript_vm_bind(vm, "EntityManager", "get_level", fun_getlevel, 0);
}




/* main state */
surgescript_var_t* fun_main(surgescript_object_t* object, const surgescript_var_t** param, int num_params)
{
    entitydb_t* db = get_db(object);

    /* clear the late update queue */
    darray_clear(db->late_update_queue);

    /* clear the brick-like object list */
    darray_clear(db->bricklike_objects);

    /* FIXME: maybe we should update the awake & detached entities AFTER unawake ones?
       e.g., camera scripts. */

    /* done */
    return NULL;
}

/* constructor */
surgescript_var_t* fun_constructor(surgescript_object_t* object, const surgescript_var_t** param, int num_params)
{
    surgescript_objectmanager_t* manager = surgescript_object_manager(object);
    surgescript_heap_t* heap = surgescript_object_heap(object);

    /* validate: Level must be the parent object */
    surgescript_objecthandle_t parent_handle = surgescript_object_parent(object);
    surgescript_object_t* parent = surgescript_objectmanager_get(manager, parent_handle);
    const char* parent_name = surgescript_object_name(parent);

    ssassert(0 == strcmp(parent_name, "Level"));

    /* allocate a database */
    entitydb_t* db = mallocx(sizeof *db);

    int lg2_cap = 15;
    db->info = fasthash_create(entityinfo_dtor, lg2_cap);
    db->id_to_handle = fasthash_create(handle_dtor, lg2_cap);
    db->cached_query = &NULL_ENTRY;

    darray_init(db->late_update_queue);
    darray_init(db->bricklike_objects);
    db->dirty_partition = false;

    db->roi.left = 0;
    db->roi.top = 0;
    db->roi.right = 0;
    db->roi.bottom = 0;

    surgescript_object_set_userdata(object, db);

    /* allocate variables */
    ssassert(AWAKEENTITYCONTAINER_ADDR == surgescript_heap_malloc(heap));
    ssassert(UNAWAKEENTITYCONTAINER_ADDR == surgescript_heap_malloc(heap));
    ssassert(DEBUGENTITYCONTAINER_ADDR == surgescript_heap_malloc(heap));
    ssassert(ENTITYTREE_ADDR == surgescript_heap_malloc(heap));
    ssassert(UNAWAKEENTITYCONTAINERARRAY_ADDR == surgescript_heap_malloc(heap));
    ssassert(NOTGARBAGECONTAINER_ADDR == surgescript_heap_malloc(heap));

    /* spawn the entity containers */
    surgescript_objecthandle_t this_handle = surgescript_object_handle(object);
    surgescript_objecthandle_t awake_container = surgescript_objectmanager_spawn(manager, this_handle, "AwakeEntityContainer", object);
    surgescript_objecthandle_t unawake_container = surgescript_objectmanager_spawn(manager, this_handle, "EntityContainer", object);
    surgescript_objecthandle_t debug_container = surgescript_objectmanager_spawn(manager, this_handle, "DebugEntityContainer", object);

    surgescript_var_set_objecthandle(surgescript_heap_at(heap, AWAKEENTITYCONTAINER_ADDR), awake_container);
    surgescript_var_set_objecthandle(surgescript_heap_at(heap, UNAWAKEENTITYCONTAINER_ADDR), unawake_container);
    surgescript_var_set_objecthandle(surgescript_heap_at(heap, DEBUGENTITYCONTAINER_ADDR), debug_container);

#if WANT_SPACE_PARTITIONING
    /* spawn the array that will store references to the unawake containers inside the region of interest */
    surgescript_objecthandle_t unawake_container_array = surgescript_objectmanager_spawn(manager, this_handle, "Array", NULL);
    surgescript_var_set_objecthandle(surgescript_heap_at(heap, UNAWAKEENTITYCONTAINERARRAY_ADDR), unawake_container_array);

    /* spawn the EntityTree */
    surgescript_objecthandle_t entity_tree = surgescript_objectmanager_spawn(manager, this_handle, "EntityTree", NULL);
    surgescript_var_set_objecthandle(surgescript_heap_at(heap, ENTITYTREE_ADDR), entity_tree);
#else
    /* unused */
    surgescript_var_set_null(surgescript_heap_at(heap, UNAWAKEENTITYCONTAINERARRAY_ADDR));
    surgescript_var_set_null(surgescript_heap_at(heap, ENTITYTREE_ADDR));
#endif

    /* spawn an object container dedicated to the prevention of garbage collection */
    surgescript_objecthandle_t notgarbage_container = surgescript_objectmanager_spawn(manager, this_handle, "PassiveLevelObjectContainer", scripting_levelobjectcontainer_token());
    surgescript_var_set_objecthandle(surgescript_heap_at(heap, NOTGARBAGECONTAINER_ADDR), notgarbage_container);

    /* done */
    return NULL;
}

/* destructor */
surgescript_var_t* fun_destructor(surgescript_object_t* object, const surgescript_var_t** param, int num_params)
{
    /* release the database */
    entitydb_t* db = get_db(object);

    darray_release(db->bricklike_objects);
    darray_release(db->late_update_queue);

    fasthash_destroy(db->id_to_handle);
    fasthash_destroy(db->info);

    free(db);

    /* done! */
    return NULL;
}

/* destroy function */
surgescript_var_t* fun_destroy(surgescript_object_t* object, const surgescript_var_t** param, int num_params)
{
    /* disabled */
    return NULL;
}

/* spawn function */
surgescript_var_t* fun_spawn(surgescript_object_t* object, const surgescript_var_t** param, int num_params)
{
    surgescript_objectmanager_t* manager = surgescript_object_manager(object);
    const char* entity_name = surgescript_var_fast_get_string(param[0]);
    surgescript_objecthandle_t entity_handle = 0;

    /* zero = Vector2(0,0) */
    surgescript_objecthandle_t zero_handle = surgescript_objectmanager_spawn_temp(manager, "Vector2");
    surgescript_object_t* zero = surgescript_objectmanager_get(manager, zero_handle);
    scripting_vector2_update(zero, 0.0, 0.0);

    /* call this.spawnEntity(setup_object_name, zero) */
    surgescript_var_t* ret = surgescript_var_create();
    surgescript_var_t* args[2] = {
        surgescript_var_set_string(surgescript_var_create(), entity_name),
        surgescript_var_set_objecthandle(surgescript_var_create(), zero_handle)
    };

    surgescript_object_call_function(object, "spawnEntity", (const surgescript_var_t**)args, 2, ret);
    entity_handle = surgescript_var_get_objecthandle(ret);

    surgescript_var_destroy(args[1]);
    surgescript_var_destroy(args[0]);
    surgescript_var_destroy(ret);

    /* done */
    surgescript_object_kill(zero);
    return surgescript_var_set_objecthandle(surgescript_var_create(), entity_handle); /* return ret; */
}

/* spawn an entity at a position in world space */
surgescript_var_t* fun_spawnentity(surgescript_object_t* object, const surgescript_var_t** param, int num_params)
{
    surgescript_heap_t* heap = surgescript_object_heap(object);
    surgescript_objectmanager_t* manager = surgescript_object_manager(object);
    const char* entity_name = surgescript_var_fast_get_string(param[0]);
    surgescript_objecthandle_t position_handle = surgescript_var_get_objecthandle(param[1]);

    /* validate: does the object exist? */
    if(!surgescript_objectmanager_class_exists(manager, entity_name)) {
        scripting_error(object, "Can't spawn entity: object \"%s\" doesn't exist!", entity_name);
        return NULL;
    }

    /* validate: accept only entities */
    surgescript_tagsystem_t* tag_system = surgescript_objectmanager_tagsystem(manager);
    if(!surgescript_tagsystem_has_tag(tag_system, entity_name, "entity")) {
        scripting_error(object, "Can't spawn entity: object \"%s\" isn't tagged \"entity\"!", entity_name);
        return NULL;
    }

    /* sanity check */
    if(
        surgescript_tagsystem_has_tag(tag_system, entity_name, "detached") &&
        !surgescript_tagsystem_has_tag(tag_system, entity_name, "private")
    ) {
        video_showmessage("Entity \"%s\" is tagged \"detached\", but not \"private\"", entity_name);
        surgescript_tagsystem_add_tag(tag_system, entity_name, "private");
    }

    /* get the Level object */
    surgescript_objecthandle_t level_handle = surgescript_object_parent(object);
    surgescript_object_t* level = surgescript_objectmanager_get(manager, level_handle);

    /* spawn the entity */
    surgescript_objecthandle_t entity_parent = level_handle;
    surgescript_objecthandle_t entity_handle = surgescript_objectmanager_spawn(manager, entity_parent, entity_name, NULL);
    surgescript_object_t* entity = surgescript_objectmanager_get(manager, entity_handle);

    /* read the spawn point */
    double spawn_x = 0.0, spawn_y = 0.0;
    surgescript_object_t* position = surgescript_objectmanager_get(manager, position_handle);
    scripting_vector2_read(position, &spawn_x, &spawn_y);
    v2d_t spawn_point = v2d_new(spawn_x, spawn_y);

    /* position the entity */
    surgescript_transform_t* transform = surgescript_object_transform(entity);
    surgescript_transform_setposition2d(transform, spawn_x, spawn_y); /* already in world space */

    /* generate entity info */
    entityinfo_t* info = entityinfo_ctor((entityinfo_t) {
        .handle = entity_handle,
        .id = generate_entity_id(),
        .spawn_point = spawn_point,
        .is_sleeping = !(
            surgescript_object_has_tag(entity, "awake") ||
            surgescript_object_has_tag(entity, "detached")
        ),
        .is_persistent = !(
            surgescript_object_has_tag(entity, "private") ||
            /*surgescript_object_has_tag(entity, "detached") ||*/ /* if it's detached, it's private - see above */
            scripting_level_issetupobjectname(level, entity_name)
        )
    });

    /* store entity info */
    entitydb_t* db = get_db(object);
    fasthash_put(db->info, info->handle, info);
    fasthash_put(db->id_to_handle, info->id, handle_ctor(info->handle));

    /* decide the entity container: is the new entity awake or not? */
    bool is_awake = (
        surgescript_tagsystem_has_tag(tag_system, entity_name, "awake") ||
        surgescript_tagsystem_has_tag(tag_system, entity_name, "detached")
    );
    surgescript_objecthandle_t entity_container_handle = surgescript_var_get_objecthandle(
        is_awake ?
        surgescript_heap_at(heap, AWAKEENTITYCONTAINER_ADDR) :
        surgescript_heap_at(heap, UNAWAKEENTITYCONTAINER_ADDR)
    );
    surgescript_object_t* entity_container = surgescript_objectmanager_get(manager, entity_container_handle);

#if WANT_SPACE_PARTITIONING
    if(!is_awake) {
        /* store the entity in a container of the EntityTree if unawake */
        surgescript_var_t* entity_tree_var = surgescript_heap_at(heap, ENTITYTREE_ADDR);
        surgescript_objecthandle_t entity_tree_handle = surgescript_var_get_objecthandle(entity_tree_var);
        surgescript_object_t* entity_tree = surgescript_objectmanager_get(manager, entity_tree_handle);
        surgescript_var_t* entity_var = surgescript_var_create();
        const surgescript_var_t* args[] = { entity_var };

        /* call entityTree.bubbleDown(entity) */
        surgescript_var_set_objecthandle(entity_var, entity_handle);
        surgescript_object_call_function(entity_tree, "bubbleDown", args, 1, NULL);

        surgescript_var_destroy(entity_var);

        /* new subsectors may have been allocated;
           mark the space partition as dirty */
        db->dirty_partition = true;
    }
    else {
        /* store the entity in the awake container */
        surgescript_var_t* arg = surgescript_var_create();
        const surgescript_var_t* args[] = { arg };

        /* call entityContainer.storeEntity(entity) */
        surgescript_var_set_objecthandle(arg, entity_handle);
        surgescript_object_call_function(entity_container, "storeEntity", args, 1, NULL);

        surgescript_var_destroy(arg);
    }
#else
    /* store the entity in the selected entity container */
    surgescript_var_t* arg = surgescript_var_create();
    const surgescript_var_t* args[] = { arg };

    /* call entityContainer.storeEntity(entity) */
    surgescript_var_set_objecthandle(arg, entity_handle);
    surgescript_object_call_function(entity_container, "storeEntity", args, 1, NULL);

    surgescript_var_destroy(arg);
#endif

    /* prevent garbage collection */
    prevent_garbage_collection(object, entity_handle);

    /* apply backwards-compatibility fix */
    inspect_subtree(entity, true, manager, tag_system, 0);

    /* return the handle to the spawned entity */
    return surgescript_var_set_objecthandle(surgescript_var_create(), entity_handle);
}

/* get the entity with the given id */
surgescript_var_t* fun_entity(surgescript_object_t* object, const surgescript_var_t** param, int num_params)
{
    surgescript_objectmanager_t* manager = surgescript_object_manager(object);
    const char* entity_id = surgescript_var_fast_get_string(param[0]);
    surgescript_objecthandle_t entity_handle = entitymanager_find_entity_by_id(object, str_to_x64(entity_id));
    surgescript_objecthandle_t null_handle = surgescript_objectmanager_null(manager);
    surgescript_var_t* ret = surgescript_var_create();

    if(entity_handle == null_handle)
        return surgescript_var_set_null(ret);
    else
        return surgescript_var_set_objecthandle(ret, entity_handle);
}

/* get the id of the given entity */
surgescript_var_t* fun_entityid(surgescript_object_t* object, const surgescript_var_t** param, int num_params)
{
    surgescript_objecthandle_t entity_handle = surgescript_var_get_objecthandle(param[0]);
    surgescript_var_t* ret = surgescript_var_create();

    if(entitymanager_has_entity_info(object, entity_handle)) {
        /* return the ID */
        char buffer[32];
        uint64_t entity_id = entitymanager_get_entity_id(object, entity_handle);
        return surgescript_var_set_string(ret, x64_to_str(entity_id, buffer, sizeof(buffer)));
    }
    else {
        /* ID not found */
        return surgescript_var_set_string(ret, "");
    }
}

/* find by name an entity that was spawned with this.spawnEntity() */
surgescript_var_t* fun_findentity(surgescript_object_t* object, const surgescript_var_t** param, int num_params)
{
    /* we first check if the object exists and if it's an entity
       it it passes those tests, then we search for it */
    surgescript_objectmanager_t* manager = surgescript_object_manager(object);
    surgescript_tagsystem_t* tag_system = surgescript_objectmanager_tagsystem(manager);
    const char* object_name = surgescript_var_fast_get_string(param[0]);
    surgescript_var_t* ret = surgescript_var_create();

    if(surgescript_tagsystem_has_tag(tag_system, object_name, "entity")) {
        /*
         * TODO: develop a faster data structure?
         * We just call Level.child() here
         */

        /* get the Level object */
        surgescript_objecthandle_t level_handle = surgescript_object_parent(object);
        surgescript_object_t* level = surgescript_objectmanager_get(manager, level_handle);

        /* find the entity */
        surgescript_object_call_function(level, "child", param, 1, ret);
        return ret; /* will be null if no entity is found */
    }
    else {
        /* the object doesn't exist or is not an entity */
        return surgescript_var_set_null(ret);
    }
}

/* find all entities with the given name that were spawned with this.spawnEntity() */
surgescript_var_t* fun_findentities(surgescript_object_t* object, const surgescript_var_t** param, int num_params)
{
    /* we first check if the objects exist and if they're entities
       it they pass those tests, then we search for it */
    surgescript_objectmanager_t* manager = surgescript_object_manager(object);
    surgescript_tagsystem_t* tag_system = surgescript_objectmanager_tagsystem(manager);
    const char* object_name = surgescript_var_fast_get_string(param[0]);
    surgescript_var_t* ret = surgescript_var_create();

    if(surgescript_tagsystem_has_tag(tag_system, object_name, "entity")) {
        /*
         * TODO: develop a faster data structure?
         * We just call Level.children() here
         */

        /* get the Level object */
        surgescript_objecthandle_t level_handle = surgescript_object_parent(object);
        surgescript_object_t* level = surgescript_objectmanager_get(manager, level_handle);

        /* find the entities */
        surgescript_object_call_function(level, "children", param, 1, ret);
        return ret; /* will be null if no entities are found */
    }
    else {
        /* the object doesn't exist or is not an entity */
        surgescript_objecthandle_t empty_array = surgescript_objectmanager_spawn_array(manager);
        return surgescript_var_set_objecthandle(ret, empty_array);
    }
}

/* get active entities: those that are inside the region of interest, as well as the awake (and detached) ones */
surgescript_var_t* fun_activeentities(surgescript_object_t* object, const surgescript_var_t** param, int num_params)
{
    surgescript_heap_t* heap = surgescript_object_heap(object);
    surgescript_objectmanager_t* manager = surgescript_object_manager(object);

    /* selectActiveEntities() takes two parameters: object[] output_array, bool skip_inactive_objects */
    bool skip_inactive_objects = !(level_editmode() || is_in_debug_mode(object));
    surgescript_objecthandle_t array_handle = surgescript_objectmanager_spawn_array(manager);
    surgescript_var_t* array_var = surgescript_var_set_objecthandle(surgescript_var_create(), array_handle);
    surgescript_var_t* skip_inactive_var = surgescript_var_set_bool(surgescript_var_create(), skip_inactive_objects);
    const surgescript_var_t* args[] = { array_var, skip_inactive_var };

    /* get awake entities */
    surgescript_var_t* awake_container_var = surgescript_heap_at(heap, AWAKEENTITYCONTAINER_ADDR);
    surgescript_objecthandle_t awake_container_handle = surgescript_var_get_objecthandle(awake_container_var);
    surgescript_object_t* awake_container = surgescript_objectmanager_get(manager, awake_container_handle);
    surgescript_object_call_function(awake_container, "selectActiveEntities", args, 2, NULL);

#if WANT_SPACE_PARTITIONING
    /* get unawakened active entities */
    foreach_unawake_container_inside_roi(object, "selectActiveEntities", args, 2);
#else
    /* get unawakened active entities */
    surgescript_var_t* unawake_container_var = surgescript_heap_at(heap, UNAWAKEENTITYCONTAINER_ADDR);
    surgescript_objecthandle_t unawake_container_handle = surgescript_var_get_objecthandle(unawake_container_var);
    surgescript_object_t* unawake_container = surgescript_objectmanager_get(manager, unawake_container_handle);
    surgescript_object_call_function(unawake_container, "selectActiveEntities", args, 2, NULL);
#endif

    /* done */
    surgescript_var_destroy(skip_inactive_var);
    return array_var;
}

/* set the current region of interest (x, y, width, height) in world coordinates */
surgescript_var_t* fun_setroi(surgescript_object_t* object, const surgescript_var_t** param, int num_params)
{
    entitydb_t* db = get_db(object);
    double x = surgescript_var_get_number(param[0]);
    double y = surgescript_var_get_number(param[1]);
    double width = surgescript_var_get_number(param[2]);
    double height = surgescript_var_get_number(param[3]);

    int left = x;
    int top = y;
    int right = x + max(width, 1.0) - 1.0;
    int bottom = y + max(height, 1.0) - 1.0;

    /* no need to update the ROI?
       save some processing time */
#if WANT_SPACE_PARTITIONING
    if(!db->dirty_partition) {
#else
    if(1) {
#endif
        if(db->roi.left == left && db->roi.top == top && db->roi.right == right && db->roi.bottom == bottom)
            return NULL;
    }

    /* set the coordinates of the ROI */
    db->roi.left = left;
    db->roi.top = top;
    db->roi.right = right;
    db->roi.bottom = bottom;

    /* maintain the entity tree */
    refresh_entity_tree(object);

    /* done */
    return NULL;
}

/* add an entity to the late update queue */
surgescript_var_t* fun_addtolateupdatequeue(surgescript_object_t* object, const surgescript_var_t** param, int num_params)
{
    entitydb_t* db = get_db(object);
    surgescript_objecthandle_t handle = surgescript_var_get_objecthandle(param[0]);

    darray_push(db->late_update_queue, handle);

    return NULL;
}

/* add a brick-like object */
surgescript_var_t* fun_addbricklikeobject(surgescript_object_t* object, const surgescript_var_t** param, int num_params)
{
    const surgescript_objectmanager_t* manager = surgescript_object_manager(object);
    surgescript_objecthandle_t handle = surgescript_var_get_objecthandle(param[0]);

    /* validate the input object */
    if(!surgescript_objectmanager_exists(manager, handle))
        return NULL;

    /* validate the object before adding it to the list */
    const surgescript_object_t* bricklike = surgescript_objectmanager_get(manager, handle);
    if(scripting_brick_is_valid(bricklike) && scripting_brick_enabled(bricklike)) {
        entitydb_t* db = get_db(object);
        darray_push(db->bricklike_objects, handle);
    }

    return NULL;
}

/* late update */
surgescript_var_t* fun_lateupdate(surgescript_object_t* object, const surgescript_var_t** param, int num_params)
{
    const surgescript_objectmanager_t* manager = surgescript_object_manager(object);
    const entitydb_t* db = get_db(object);

    /* for each entity in the late update queue, call entity.lateUpdate() */
    for(int i = 0; i < darray_length(db->late_update_queue); i++) {
        surgescript_objecthandle_t entity_handle = db->late_update_queue[i];
        if(surgescript_objectmanager_exists(manager, entity_handle)) { /* validity check */
            surgescript_object_t* entity = surgescript_objectmanager_get(manager, entity_handle);
            if(!surgescript_object_is_killed(entity)) {
                surgescript_object_call_function(entity, "lateUpdate", NULL, 0, NULL);
            }
        }
    }

    /* done! */
    return NULL;
}

/* notify entities: given the name of a function with no arguments, call it in all entities */
surgescript_var_t* fun_notifyentities(surgescript_object_t* object, const surgescript_var_t** param, int num_params)
{
    const surgescript_heap_t* heap = surgescript_object_heap(object);
    const surgescript_objectmanager_t* manager = surgescript_object_manager(object);

    /* notify entities of the debug container */
    surgescript_var_t* debug_container_var = surgescript_heap_at(heap, DEBUGENTITYCONTAINER_ADDR);
    surgescript_objecthandle_t debug_container_handle = surgescript_var_get_objecthandle(debug_container_var);
    surgescript_object_t* debug_container = surgescript_objectmanager_get(manager, debug_container_handle);
    surgescript_object_call_function(debug_container, "notifyEntities", param, num_params, NULL);

    /* notify entities of the awake container */
    surgescript_var_t* awake_container_var = surgescript_heap_at(heap, AWAKEENTITYCONTAINER_ADDR);
    surgescript_objecthandle_t awake_container_handle = surgescript_var_get_objecthandle(awake_container_var);
    surgescript_object_t* awake_container = surgescript_objectmanager_get(manager, awake_container_handle);
    surgescript_object_call_function(awake_container, "notifyEntities", param, num_params, NULL);

#if WANT_SPACE_PARTITIONING
    /* notify entities of all unawake containers */
    foreach_unawake_container(object, "notifyEntities", param, num_params);
#else
    /* notify entities of the unawake container */
    surgescript_var_t* unawake_container_var = surgescript_heap_at(heap, UNAWAKEENTITYCONTAINER_ADDR);
    surgescript_objecthandle_t unawake_container_handle = surgescript_var_get_objecthandle(unawake_container_var);
    surgescript_object_t* unawake_container = surgescript_objectmanager_get(manager, unawake_container_handle);
    surgescript_object_call_function(unawake_container, "notifyEntities", param, num_params, NULL);
#endif

    /* done! */
    return NULL;
}

/* render the entities */
surgescript_var_t* fun_render(surgescript_object_t* object, const surgescript_var_t** param, int num_params)
{
    const surgescript_objectmanager_t* manager = surgescript_object_manager(object);
    const surgescript_heap_t* heap = surgescript_object_heap(object);
    surgescript_var_t* arg = surgescript_var_create();
    const surgescript_var_t* args[] = { arg };

    /* set the rendering flags */
    int flags = 0;
    flags |= (level_editmode() || is_in_debug_mode(object)) ? 0x1 : 0;
    flags |= level_is_displaying_gizmos() ? 0x2 : 0;
    surgescript_var_set_rawbits(arg, flags);

    /* render entities of the debug container */
    surgescript_var_t* debug_container_var = surgescript_heap_at(heap, DEBUGENTITYCONTAINER_ADDR);
    surgescript_objecthandle_t debug_container_handle = surgescript_var_get_objecthandle(debug_container_var);
    surgescript_object_t* debug_container = surgescript_objectmanager_get(manager, debug_container_handle);
    surgescript_object_call_function(debug_container, "render", args, 1, NULL);

    /* render entities of the awake container */
    surgescript_var_t* awake_container_var = surgescript_heap_at(heap, AWAKEENTITYCONTAINER_ADDR);
    surgescript_objecthandle_t awake_container_handle = surgescript_var_get_objecthandle(awake_container_var);
    surgescript_object_t* awake_container = surgescript_objectmanager_get(manager, awake_container_handle);
    surgescript_object_call_function(awake_container, "render", args, 1, NULL);

#if WANT_SPACE_PARTITIONING
    /* render entities of the unawake containers */
    foreach_unawake_container_inside_roi(object, "render", args, 1);
#else
    /* render entities of the unawake container */
    surgescript_var_t* unawake_container_var = surgescript_heap_at(heap, UNAWAKEENTITYCONTAINER_ADDR);
    surgescript_objecthandle_t unawake_container_handle = surgescript_var_get_objecthandle(unawake_container_var);
    surgescript_object_t* unawake_container = surgescript_objectmanager_get(manager, unawake_container_handle);
    surgescript_object_call_function(unawake_container, "render", args, 1, NULL);
#endif

    /* done */
    surgescript_var_destroy(arg);
    return NULL;
}

/* are we in the Debug Mode? */
surgescript_var_t* fun_isindebugmode(surgescript_object_t* object, const surgescript_var_t** param, int num_params)
{
    /* this routine should be fast */
    const surgescript_objectmanager_t* manager = surgescript_object_manager(object);
    const surgescript_heap_t* heap = surgescript_object_heap(object);

    /* get the debug container */
    const surgescript_var_t* debug_container_var = surgescript_heap_at(heap, DEBUGENTITYCONTAINER_ADDR);
    surgescript_objecthandle_t debug_container_handle = surgescript_var_get_objecthandle(debug_container_var);
    surgescript_object_t* debug_container = surgescript_objectmanager_get(manager, debug_container_handle);

    /* delegate */
    surgescript_var_t* ret = surgescript_var_create();
    surgescript_object_call_function(debug_container, "isInDebugMode", NULL, 0, ret);
    return ret;
}

/* enter the Debug Mode */
surgescript_var_t* fun_enterdebugmode(surgescript_object_t* object, const surgescript_var_t** param, int num_params)
{
    const surgescript_objectmanager_t* manager = surgescript_object_manager(object);
    const surgescript_heap_t* heap = surgescript_object_heap(object);

    /* get the debug container */
    const surgescript_var_t* debug_container_var = surgescript_heap_at(heap, DEBUGENTITYCONTAINER_ADDR);
    surgescript_objecthandle_t debug_container_handle = surgescript_var_get_objecthandle(debug_container_var);
    surgescript_object_t* debug_container = surgescript_objectmanager_get(manager, debug_container_handle);

    /* delegate */
    surgescript_object_call_function(debug_container, "enterDebugMode", NULL, 0, NULL);
    return NULL;
}

/* exit the Debug Mode */
surgescript_var_t* fun_exitdebugmode(surgescript_object_t* object, const surgescript_var_t** param, int num_params)
{
    const surgescript_objectmanager_t* manager = surgescript_object_manager(object);
    const surgescript_heap_t* heap = surgescript_object_heap(object);

    /* get the debug container */
    const surgescript_var_t* debug_container_var = surgescript_heap_at(heap, DEBUGENTITYCONTAINER_ADDR);
    surgescript_objecthandle_t debug_container_handle = surgescript_var_get_objecthandle(debug_container_var);
    surgescript_object_t* debug_container = surgescript_objectmanager_get(manager, debug_container_handle);

    /* delegate */
    surgescript_object_call_function(debug_container, "exitDebugMode", NULL, 0, NULL);
    return NULL;
}

/* get the Debug Mode object (may be null) */
surgescript_var_t* fun_getdebugmode(surgescript_object_t* object, const surgescript_var_t** param, int num_params)
{
    const surgescript_objectmanager_t* manager = surgescript_object_manager(object);
    const surgescript_heap_t* heap = surgescript_object_heap(object);

    /* get the debug container */
    const surgescript_var_t* debug_container_var = surgescript_heap_at(heap, DEBUGENTITYCONTAINER_ADDR);
    surgescript_objecthandle_t debug_container_handle = surgescript_var_get_objecthandle(debug_container_var);
    surgescript_object_t* debug_container = surgescript_objectmanager_get(manager, debug_container_handle);

    /* delegate */
    surgescript_var_t* ret = surgescript_var_create();
    surgescript_object_call_function(debug_container, "get_debugMode", NULL, 0, ret);
    return ret;
}

/* pause the entity containers */
surgescript_var_t* fun_pausecontainers(surgescript_object_t* object, const surgescript_var_t** param, int num_params)
{
    pause_containers(object, true);
    return NULL;
}

/* resume the entity containers */
surgescript_var_t* fun_resumecontainers(surgescript_object_t* object, const surgescript_var_t** param, int num_params)
{
    pause_containers(object, false);
    return NULL;
}

/* get a reference to the Level object */
surgescript_var_t* fun_getlevel(surgescript_object_t* object, const surgescript_var_t** param, int num_params)
{
    const surgescript_objectmanager_t* manager = surgescript_object_manager(object);
    surgescript_objecthandle_t parent_handle = surgescript_object_parent(object);

    /* validate: Level must be the parent object */
    const surgescript_object_t* parent = surgescript_objectmanager_get(manager, parent_handle);
    const char* parent_name = surgescript_object_name(parent);
    ssassert(0 == strcmp(parent_name, "Level"));

    /* done */
    return surgescript_var_set_objecthandle(surgescript_var_create(), parent_handle);
}



/*
 *
 * C API
 * 
 */

/* do we have the info of the given entity? */
bool entitymanager_has_entity_info(surgescript_object_t* entity_manager, surgescript_objecthandle_t entity_handle)
{
    return quick_lookup(entity_manager, entity_handle) != NULL;
}

/* remove entity info */
void entitymanager_remove_entity_info(surgescript_object_t* entity_manager, surgescript_objecthandle_t entity_handle)
{
    entityinfo_t* info = quick_lookup(entity_manager, entity_handle);

    if(info != NULL) {
        entitydb_t* db = get_db(entity_manager);
        uint64_t entity_id = info->id;

        fasthash_delete(db->id_to_handle, entity_id);
        fasthash_delete(db->info, entity_handle);
    }
}

/* get the ID of an entity */
uint64_t entitymanager_get_entity_id(surgescript_object_t* entity_manager, surgescript_objecthandle_t entity_handle)
{
    entityinfo_t* info = quick_lookup(entity_manager, entity_handle);
    return info != NULL ? info->id : 0;
}

/* change the ID of an entity */
void entitymanager_set_entity_id(surgescript_object_t* entity_manager, surgescript_objecthandle_t entity_handle, uint64_t entity_id)
{
    entityinfo_t* info = quick_lookup(entity_manager, entity_handle);

    if(info != NULL) {
        entitydb_t* db = get_db(entity_manager);

        /* update the id_to_handle hashtable */
        fasthash_delete(db->id_to_handle, info->id);
        fasthash_put(db->id_to_handle, entity_id, handle_ctor(info->handle));

        /* set the new id */
        info->id = entity_id;
    }
}

/* get the spawn point of an entity */
v2d_t entitymanager_get_entity_spawn_point(surgescript_object_t* entity_manager, surgescript_objecthandle_t entity_handle)
{
    entityinfo_t* info = quick_lookup(entity_manager, entity_handle);
    return info != NULL ? info->spawn_point : v2d_new(0, 0);
}

/* is the entity persistent? */
bool entitymanager_is_entity_persistent(surgescript_object_t* entity_manager, surgescript_objecthandle_t entity_handle)
{
    entityinfo_t* info = quick_lookup(entity_manager, entity_handle);
    return info != NULL ? info->is_persistent : false; /* return false if the entity info is missing */
}

/* change the persistent flag of an entity */
void entitymanager_set_entity_persistent(surgescript_object_t* entity_manager, surgescript_objecthandle_t entity_handle, bool is_persistent)
{
    entityinfo_t* info = quick_lookup(entity_manager, entity_handle);
    if(info != NULL)
        info->is_persistent = is_persistent;
}

/* is the entity sleeping? */
bool entitymanager_is_entity_sleeping(surgescript_object_t* entity_manager, surgescript_objecthandle_t entity_handle)
{
    entityinfo_t* info = quick_lookup(entity_manager, entity_handle);
    return info != NULL ? info->is_sleeping : true;
}

/* change the sleeping flag of an entity */
void entitymanager_set_entity_sleeping(surgescript_object_t* entity_manager, surgescript_objecthandle_t entity_handle, bool is_sleeping)
{
    entityinfo_t* info = quick_lookup(entity_manager, entity_handle);
    if(info != NULL)
        info->is_sleeping = is_sleeping;
}

/* find entity by ID. This may return a null handle! */
surgescript_objecthandle_t entitymanager_find_entity_by_id(surgescript_object_t* entity_manager, uint64_t entity_id)
{
    entitydb_t* db = get_db(entity_manager);
    surgescript_objecthandle_t* handle_ptr = (surgescript_objecthandle_t*)fasthash_get(db->id_to_handle, entity_id);
    surgescript_objectmanager_t* manager = surgescript_object_manager(entity_manager);

    if(handle_ptr == NULL) {
        /* ID not found */
        return surgescript_objectmanager_null(manager);
    }
    else if(!surgescript_objectmanager_exists(manager, *handle_ptr)) {
        /* the entity no longer exists */
        entitymanager_remove_entity_info(entity_manager, *handle_ptr);
        return surgescript_objectmanager_null(manager);
    }
    else {
        /* success! */
        return *handle_ptr;
    }
}

/* check if a position is inside the region of interest */
bool entitymanager_is_inside_roi(surgescript_object_t* entity_manager, v2d_t position)
{
    const entitydb_t* db = get_db(entity_manager);
    int x = position.x, y = position.y;

    return x >= db->roi.left && x <= db->roi.right && y >= db->roi.top && y <= db->roi.bottom;
}

/* get the (inclusive) coordinates of the region of interest */
void entitymanager_get_roi(surgescript_object_t* entity_manager, int* top, int* left, int* bottom, int* right)
{
    const entitydb_t* db = get_db(entity_manager);

    *top = db->roi.top;
    *left = db->roi.left;
    *bottom = db->roi.bottom;
    *right = db->roi.right;
}

/* create an iterator for iterating over the collection of (handles of) brick-like objects */
iterator_t* entitymanager_bricklike_iterator(surgescript_object_t* entity_manager)
{
    entitydb_t* db = get_db(entity_manager);

    return iterator_create_from_array(
        db->bricklike_objects,
        darray_length(db->bricklike_objects),
        sizeof *(db->bricklike_objects)
    );
}

/* create an iterator for iterating over the collection of (handles of) active entities
   (i.e., awake, inside the ROI...) */
iterator_t* entitymanager_activeentities_iterator(surgescript_object_t* entity_manager)
{
    surgescript_objectmanager_t* manager = surgescript_object_manager(entity_manager);

    /* call entityManager.activeEntities(), which returns a temporary SurgeScript Array */
    surgescript_var_t* ret = surgescript_var_create();
    surgescript_object_call_function(entity_manager, "activeEntities", NULL, 0, ret);
    surgescript_objecthandle_t array_handle = surgescript_var_get_objecthandle(ret);
    surgescript_var_destroy(ret);

    /* sanity check */
    if(!surgescript_objectmanager_exists(manager, array_handle))
        return iterator_create_from_array(NULL, 0, 0); /* empty */

    /* iterate over the temporary SurgeScript Array */
    surgescript_object_t* array = surgescript_objectmanager_get(manager, array_handle);
    return iterator_create_from_disposable_surgescript_array(array);
}



/*
 *
 * Helpers
 * 
 */

/* performs a quick lookup */
entityinfo_t* quick_lookup(surgescript_object_t* entity_manager, surgescript_objecthandle_t entity_handle)
{
    /* this gotta be fast! */
    entitydb_t* db = get_db(entity_manager);

    /* the rationale for caching is that we tend to make multiple
       queries related to the same entity sequentially in time */
    if(db->cached_query->handle != entity_handle) {
        entityinfo_t* info = get_info(db, entity_handle); /* NULL if not found */
        return info != NULL ? (db->cached_query = info) : NULL;
    }

    /* the entry was cached */
    return db->cached_query;
}

/* calls a function on each unawake container inside the region of interest */
void foreach_unawake_container_inside_roi(surgescript_object_t* entity_manager, const char* fun_name, const surgescript_var_t** param, int num_params)
{
    surgescript_heap_t* heap = surgescript_object_heap(entity_manager);
    surgescript_objectmanager_t* manager = surgescript_object_manager(entity_manager);

    /* get the array of unawake containers inside the ROI */
    surgescript_var_t* array_var = surgescript_heap_at(heap, UNAWAKEENTITYCONTAINERARRAY_ADDR);
    surgescript_objecthandle_t array_handle = surgescript_var_get_objecthandle(array_var);
    surgescript_object_t* array = surgescript_objectmanager_get(manager, array_handle);

    /* for each unawake container */
    iterator_t* it = iterator_create_from_surgescript_array(array);
    while(iterator_has_next(it)) {
        surgescript_var_t** container_var = iterator_next(it);
        surgescript_objecthandle_t container_handle = surgescript_var_get_objecthandle(*container_var);
        surgescript_object_t* container = surgescript_objectmanager_get(manager, container_handle);

        /* call function */
        surgescript_object_call_function(container, fun_name, param, num_params, NULL);
    }
    iterator_destroy(it);
}

/* calls a function on all unawake containers */
void foreach_unawake_container(surgescript_object_t* entity_manager, const char* fun_name, const surgescript_var_t** param, int num_params)
{
    surgescript_heap_t* heap = surgescript_object_heap(entity_manager);
    surgescript_objectmanager_t* manager = surgescript_object_manager(entity_manager);

    /* get the EntityTree */
    surgescript_var_t* entity_tree_var = surgescript_heap_at(heap, ENTITYTREE_ADDR);
    surgescript_objecthandle_t entity_tree_handle = surgescript_var_get_objecthandle(entity_tree_var);
    surgescript_object_t* entity_tree = surgescript_objectmanager_get(manager, entity_tree_handle);

    /* for each unawake container in the EntityTree */
    void* pack[] = { manager, (char*)fun_name, param, &num_params };
    surgescript_object_find_descendants(entity_tree, "EntityContainer", pack, foreach_unawake_container_callback);
    /* ^ slow!!! */
}

/* callback for foreach_unawake_container() */
void foreach_unawake_container_callback(surgescript_objecthandle_t container_handle, void* data)
{
    void** pack = (void**)data;

    surgescript_objectmanager_t* manager = pack[0];
    char* fun_name = pack[1];
    const surgescript_var_t** param = pack[2];
    int* num_params = pack[3];

    /* get the unawake container */
    surgescript_object_t* container = surgescript_objectmanager_get(manager, container_handle);

    /* call function */
    surgescript_object_call_function(container, fun_name, param, *num_params, NULL);
}

/* pause containers */
void pause_containers(surgescript_object_t* entity_manager, bool pause)
{
    surgescript_objectmanager_t* manager = surgescript_object_manager(entity_manager);
    surgescript_heap_t* heap = surgescript_object_heap(entity_manager);
    const char* fun_name = pause ? "pause" : "resume";

    /* pause the awake container */
    surgescript_var_t* awake_container_var = surgescript_heap_at(heap, AWAKEENTITYCONTAINER_ADDR);
    surgescript_objecthandle_t awake_container_handle = surgescript_var_get_objecthandle(awake_container_var);
    surgescript_object_t* awake_container = surgescript_objectmanager_get(manager, awake_container_handle);
    surgescript_object_call_function(awake_container, fun_name, NULL, 0, NULL);

    /* pause the unawake container */
    surgescript_var_t* unawake_container_var = surgescript_heap_at(heap, UNAWAKEENTITYCONTAINER_ADDR);
    surgescript_objecthandle_t unawake_container_handle = surgescript_var_get_objecthandle(unawake_container_var);
    surgescript_object_t* unawake_container = surgescript_objectmanager_get(manager, unawake_container_handle);
    surgescript_object_call_function(unawake_container, fun_name, NULL, 0, NULL);

#if WANT_SPACE_PARTITIONING
    /* pause the EntityTree */
    surgescript_var_t* entity_tree_var = surgescript_heap_at(heap, ENTITYTREE_ADDR);
    surgescript_objecthandle_t entity_tree_handle = surgescript_var_get_objecthandle(entity_tree_var);
    surgescript_object_t* entity_tree = surgescript_objectmanager_get(manager, entity_tree_handle);
    surgescript_object_set_active(entity_tree, !pause); /* TODO */
#endif

    /* do not pause the debug container */
    ;
}

/* are we in the Debug Mode? */
bool is_in_debug_mode(surgescript_object_t* entity_manager)
{
    surgescript_var_t* ret = surgescript_var_create();

    surgescript_object_call_function(entity_manager, "isInDebugMode", NULL, 0, ret);
    bool value = surgescript_var_get_bool(ret);

    surgescript_var_destroy(ret);
    return value;
}

/* refresh the entity tree: partition the space and update the unawake entity container array */
void refresh_entity_tree(surgescript_object_t* entity_manager)
{
#if WANT_SPACE_PARTITIONING
    const surgescript_objectmanager_t* manager = surgescript_object_manager(entity_manager);
    const surgescript_heap_t* heap = surgescript_object_heap(entity_manager);
    entitydb_t* db = get_db(entity_manager);

    /* get the entity tree */
    surgescript_var_t* entity_tree_var = surgescript_heap_at(heap, ENTITYTREE_ADDR);
    surgescript_objecthandle_t entity_tree_handle = surgescript_var_get_objecthandle(entity_tree_var);
    surgescript_object_t* entity_tree = surgescript_objectmanager_get(manager, entity_tree_handle);

    /* get the unawake entity container array */
    surgescript_var_t* unawake_container_array_var = surgescript_heap_at(heap, UNAWAKEENTITYCONTAINERARRAY_ADDR);
    surgescript_objecthandle_t unawake_container_array_handle = surgescript_var_get_objecthandle(unawake_container_array_var);
    surgescript_object_t* unawake_container_array = surgescript_objectmanager_get(manager, unawake_container_array_handle);

    /* bubble up entities (from the previous update cycle) */
    iterator_t* it = iterator_create_from_surgescript_array(unawake_container_array);
    while(iterator_has_next(it)) {
        surgescript_var_t** unawake_container_var = iterator_next(it);
        surgescript_objecthandle_t unawake_container_handle = surgescript_var_get_objecthandle(*unawake_container_var);
        surgescript_object_t* unawake_container = surgescript_objectmanager_get(manager, unawake_container_handle);

        surgescript_object_call_function(unawake_container, "bubbleUpEntities", NULL, 0, NULL);
    }
    iterator_destroy(it);

    /* update the size of the world */
    v2d_t world_size = level_size();
    surgescript_var_t* world_width_var = surgescript_var_set_number(surgescript_var_create(), world_size.x);
    surgescript_var_t* world_height_var = surgescript_var_set_number(surgescript_var_create(), world_size.y);
    surgescript_var_t* world_size_has_changed = surgescript_var_create();

    const surgescript_var_t* world_size_args[] = { world_width_var, world_height_var };
    surgescript_object_call_function(entity_tree, "updateWorldSize", world_size_args, 2, world_size_has_changed);

    if(surgescript_var_get_bool(world_size_has_changed)) {
        /* if the world size has changed, then we must
           relocate all entities of all containers */
        logfile_message("EntityManager: world size has changed. Relocating all entities...");
        foreach_unawake_container(entity_manager, "bubbleUpEntities", NULL, 0);
    }

    surgescript_var_destroy(world_size_has_changed);
    surgescript_var_destroy(world_height_var);
    surgescript_var_destroy(world_width_var);

    /* clear the unawake entity container array */
    surgescript_object_call_function(unawake_container_array, "clear", NULL, 0, NULL);

    /* update the ROI of the entity tree, as well as the unawake container array */
    surgescript_var_t* output_array_var = surgescript_var_clone(unawake_container_array_var);
    surgescript_var_t* top_var = surgescript_var_set_number(surgescript_var_create(), db->roi.top);
    surgescript_var_t* left_var = surgescript_var_set_number(surgescript_var_create(), db->roi.left);
    surgescript_var_t* bottom_var = surgescript_var_set_number(surgescript_var_create(), db->roi.bottom);
    surgescript_var_t* right_var = surgescript_var_set_number(surgescript_var_create(), db->roi.right);

    const surgescript_var_t* args[] = { output_array_var, top_var, left_var, bottom_var, right_var };
    surgescript_object_call_function(entity_tree, "updateROI", args, 5, NULL);

    surgescript_var_destroy(right_var);
    surgescript_var_destroy(bottom_var);
    surgescript_var_destroy(left_var);
    surgescript_var_destroy(top_var);
    surgescript_var_destroy(output_array_var);

    /* the space partition is clean again, i.e.,
       the unawake entity container array has the correct entries */
    db->dirty_partition = false;
#else

    /* no space partitioning */
    (void)foreach_unawake_container;
    (void)foreach_unawake_container_callback;
    (void)foreach_unawake_container_inside_roi;

#endif
}

/* Check if the subtree whose root is "root" has any descendants tagged
   "entity" that are not direct children of "root". If so, tag them as a
   temporary fix, for backwards compatibility with Open Surge 0.6.0.x or
   earlier. In addition, warn the user.

   This traverses the subtree. It is only applied at spawn time. This should
   be fixed manually by the user. */
bool inspect_subtree(const surgescript_object_t* root, bool is_root_entity, const surgescript_objectmanager_t* manager, surgescript_tagsystem_t* tag_system, int depth)
{
    /* do some pruning. this is just a diagnostic tool and we're not supposed
       to waste time here */
    const int MAX_DEPTH = 2; /* depth = 0, 1, 2... */
    int new_depth = 1 + depth;

    bool fixed_descendant = false, fixed_root = false;
    int n = surgescript_object_child_count(root);

    for(int i = 0; i < n; i++) {
        surgescript_objecthandle_t child_handle = surgescript_object_nth_child(root, i);

        if(surgescript_objectmanager_exists(manager, child_handle)) {
            const surgescript_object_t* child = surgescript_objectmanager_get(manager, child_handle);
            bool is_child_entity = surgescript_object_has_tag(child, "entity");

            /* we found an object that is not an entity and that has a child tagged "entity".
               This is a violation of the definition. */
            if(!is_root_entity && is_child_entity) {

                /* make the object an entity as a temporary fix */
                const char* root_name = surgescript_object_name(root);
                surgescript_tagsystem_add_tag(tag_system, root_name, "entity");
                surgescript_tagsystem_add_tag(tag_system, root_name, "private");
                surgescript_tagsystem_add_tag(tag_system, root_name, "awake");
                surgescript_tagsystem_add_tag(tag_system, root_name, "detached");

                /* warn the user */
                const char* child_name = surgescript_object_name(child);
                video_showmessage("\"%s\" violates the definition of entity", child_name);
                logfile_message("EntityManager: \"%s\" violates the definition of entity", child_name);

                #if 0
                /* look deeper */
                new_depth = 0;
                #else
                /* don't look deeper. this is just a diagnostic for the modder
                   and should not interfere with normal gameplay. */
                #endif

                /* mark the root as "fixed" */
                fixed_root = true;

            }

            /* traverse the subtree whose root is child */
            if(new_depth <= MAX_DEPTH) {
                if(inspect_subtree(child, is_child_entity, manager, tag_system, new_depth))
                    fixed_descendant = true;
            }
        }
    }

    /* ask for user intervention */
    if(fixed_root) {
        const char* root_name = surgescript_object_name(root);
        video_showmessage("\"%s\" should be tagged \"entity\"", root_name);
        logfile_message("EntityManager: \"%s\" should be tagged \"entity\"", root_name);
    }

    /* done */
    return fixed_root || fixed_descendant;
}

/* Prevent Garbage Collection

   Even though entities are stored in entity containers, the reference links are continuously changing.
   Due to the nature of the incremental mark-and-sweep garbage collection method currently implemented
   in SurgeScript (version 0.6.0 at the time of this writing), entities may be accidentally removed
   because the links may be changed while the algorithm is collecting data. Therefore, we will keep
   new links in a different container and we will not change these. */
void prevent_garbage_collection(surgescript_object_t* entity_manager, surgescript_objecthandle_t entity_handle)
{
    surgescript_objectmanager_t* manager = surgescript_object_manager(entity_manager);
    surgescript_heap_t* heap = surgescript_object_heap(entity_manager);

    surgescript_var_t* container_var = surgescript_heap_at(heap, NOTGARBAGECONTAINER_ADDR);
    surgescript_objecthandle_t container_handle = surgescript_var_get_objecthandle(container_var);
    surgescript_object_t* container = surgescript_objectmanager_get(manager, container_handle);

    surgescript_var_t* param = surgescript_var_set_objecthandle(surgescript_var_create(), entity_handle);
    surgescript_object_call_function(container, "addObject", (const surgescript_var_t*[]){ param }, 1, NULL);
    surgescript_var_destroy(param);
}