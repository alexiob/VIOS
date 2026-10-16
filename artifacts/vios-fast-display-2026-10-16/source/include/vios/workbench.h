#ifndef VIOS_WORKBENCH_H
#define VIOS_WORKBENCH_H
#include "forth/modules.h"
#include "forth/inspect.h"
#define VIOS_WB_SOURCE_MAX 4096u
#define VIOS_WB_NAME_MAX 31u
enum vios_workbench_browser { VIOS_WB_MODULES, VIOS_WB_WORDS };
enum vios_workbench_focus { VIOS_WB_BROWSER, VIOS_WB_EDITOR };
enum vios_workbench_request { VIOS_WB_NONE, VIOS_WB_STAGE, VIOS_WB_ACTIVATE, VIOS_WB_SAVE, VIOS_WB_CLOSED };
/* Synthetic semantic keys; printable ASCII and TAB/CR/LF are their byte values.
 * Hardware adapters translate virtual keys into these values. */
enum vios_workbench_key { VIOS_WB_UP=256, VIOS_WB_DOWN, VIOS_WB_LEFT,
 VIOS_WB_RIGHT, VIOS_WB_HOME, VIOS_WB_END, VIOS_WB_DELETE,
 VIOS_WB_F5, VIOS_WB_F6, VIOS_WB_F7, VIOS_WB_F8, VIOS_WB_F10 };
/* Matches pinned keyboard modifier bits; ASCII is already case translated. */
enum vios_workbench_modifier { VIOS_WB_MOD_CTRL=1,VIOS_WB_MOD_SHIFT=2,
 VIOS_WB_MOD_LALT=4,VIOS_WB_MOD_RALT=8,VIOS_WB_MOD_CAPSLOCK=16,
 VIOS_WB_MOD_NUMLOCK=32,VIOS_WB_MOD_SCROLLLOCK=64,VIOS_WB_MOD_GUI=128 };
struct vios_workbench_status {
 const char *module,*version,*test_word,*error,*selection,*filter;
 const struct forth_module_dependency *dependencies;
 unsigned dependency_count,length,cursor,selection_index,browser_count;
 uint32_t revision,catalog_revision;
 int open,has_draft,dirty,staged,activated,saved,undo_available;
 enum vios_workbench_browser browser;
 enum vios_workbench_focus focus;
};
void vios_workbench_init(void);
void vios_workbench_open(void);
void vios_workbench_close(void);
int vios_workbench_active(void);
void vios_workbench_zoom(int enabled);
int vios_workbench_zoomed(void);
void vios_workbench_status(struct vios_workbench_status *out);
const char *vios_workbench_error(void);
void vios_workbench_note_error(const char *message);
const char *vios_workbench_source(void);
int vios_workbench_browse(enum vios_workbench_browser browser,const char *module_filter);
int vios_workbench_select_word(unsigned index); /* absolute catalog index */
int vios_workbench_select_module(const char *name);
int vios_workbench_selected_word(struct forth_word_info *out);
int vios_workbench_edit(const char *module,int discard_dirty);
int vios_workbench_new(const char *module,const char *version,int discard_dirty);
int vios_workbench_patch(uint32_t expected_revision,size_t offset,size_t remove,
 const char *bytes,size_t length);
int vios_workbench_undo(uint32_t expected_revision);
int vios_workbench_set_metadata(uint32_t expected_revision,const char *version,
 const char *test_word,const struct forth_module_dependency *dependencies,unsigned count);
int vios_workbench_stage(uint32_t expected_revision);
int vios_workbench_activate(uint32_t expected_revision);
/* Caller must check can_save, persist via vios_module_save, then mark_saved.
 * Both functions revalidate the exact current active source and metadata. */
int vios_workbench_can_save(uint32_t expected_revision);
int vios_workbench_mark_saved(uint32_t expected_revision);
void vios_workbench_compose(void);
enum vios_workbench_request vios_workbench_key(unsigned code,unsigned modifiers);
#endif
