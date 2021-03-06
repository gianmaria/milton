// Copyright (c) 2015-2016 Sergio Gonzalez. All rights reserved.
// License: https://github.com/serge-rgb/milton#license

#include "persist.h"

#include <stb_image_write.h>

#include "common.h"
#include "gui.h"
#include "memory.h"
#include "milton.h"
#include "platform.h"
#include "tiny_jpeg.h"


#define MILTON_MAGIC_NUMBER 0X11DECAF3

// Will allocate memory so that if the read fails, we will restore what was
// originally in there.
static b32
fread_checked(void* dst, size_t sz, size_t count, FILE* fd)
{
    b32 ok = false;

    size_t read = fread(dst, sz, count, fd);
    if ( read == count && !ferror(fd)) {
        ok = true;
    }

    return ok;
}

static b32
fwrite_checked(void* data, size_t sz, size_t count, FILE* fd)
{
    b32 ok = false;
    size_t written = fwrite(data, sz, count, fd);
    if ( written == count ) {
        ok = true;
    }

    return ok;
}

void
milton_unset_last_canvas_fname()
{
    b32 del = platform_delete_file_at_config(TO_PATH_STR("saved_path"), DeleteErrorTolerance_OK_NOT_EXIST);
    if ( del == false ) {
        platform_dialog("The default canvas could not be set to open the next time you run Milton. Please contact the developers.", "Important");
    }
}

void
milton_load(MiltonState* milton_state)
{
    // Declare variables here to silence compiler warnings about using GOTO.
    i32 history_count = 0;
    i32 num_layers = 0;
    i32 saved_working_layer_id = 0;
    int err = 0;

    i32 layer_guid = 0;
    ColorButton* btn = NULL;
    MiltonGui* gui = NULL;
    auto saved_size = milton_state->view->screen_size;

    milton_log("Milton: loading file\n");
    // Reset the canvas.
    milton_reset_canvas(milton_state);

    CanvasState* canvas = milton_state->canvas;
#define READ(address, size, num, fd) do { ok = fread_checked(address,size,num,fd); if (!ok){ goto END; } } while(0)

    // Unload gpu data if the strokes have been cooked.
    gpu_free_strokes(milton_state->render_data, milton_state->canvas);
    mlt_assert(milton_state->mlt_file_path);
    FILE* fd = platform_fopen(milton_state->mlt_file_path, TO_PATH_STR("rb"));
    b32 ok = true;  // fread check
    b32 handled = false;  // when ok==false but we don't need to prompt a scary message.

    if ( fd ) {
        u32 milton_binary_version = (u32)-1;
        u32 milton_magic = (u32)-1;
        READ(&milton_magic, sizeof(u32), 1, fd);
        READ(&milton_binary_version, sizeof(u32), 1, fd);

        if (ok) {
            if ( milton_binary_version < 4 ) {
                if ( platform_dialog_yesno ("This file will be updated to the new version of Milton. Older versions won't be able to open it. Is this OK?", "File format change") ) {
                    milton_state->mlt_binary_version = MILTON_MINOR_VERSION;
                    milton_log("Updating this file to mlt version 4.\n");
                } else {
                    ok = false;
                    handled = true;
                    goto END;
                }
            } else {
                milton_state->mlt_binary_version = milton_binary_version;
            }
        }

        if ( milton_binary_version > MILTON_MINOR_VERSION ) {
            platform_dialog("This file was created with a newer version of Milton.", "Could not open.");

            // Stop loading, but exit without prompting.
            ok = false;
            handled = true;
            goto END;
        }

        if ( milton_binary_version >= 4 ) {
            READ(milton_state->view, sizeof(CanvasView), 1, fd);
        } else {
            CanvasViewPreV4 legacy_view = {};
            READ(&legacy_view, sizeof(CanvasViewPreV4), 1, fd);
            milton_state->view->screen_size = legacy_view.screen_size;
            milton_state->view->scale = legacy_view.scale;
            milton_state->view->zoom_center = legacy_view.zoom_center;
            milton_state->view->pan_center = VEC2L(legacy_view.pan_center * -1);
            milton_state->view->background_color = legacy_view.background_color;
            milton_state->view->working_layer_id = legacy_view.working_layer_id;
            milton_state->view->num_layers = legacy_view.num_layers;
        }

        // The screen size might hurt us.
        milton_state->view->screen_size = saved_size;

        // The process of loading changes state. working_layer_id changes when creating layers.
        saved_working_layer_id = milton_state->view->working_layer_id;

        if ( milton_magic != MILTON_MAGIC_NUMBER ) {
            platform_dialog("MLT file could not be loaded. Magic number mismatch.", "Problem");
            milton_unset_last_canvas_fname();
            ok = false;
            goto END;
        }

        num_layers = 0;
        READ(&num_layers, sizeof(i32), 1, fd);
        READ(&layer_guid, sizeof(i32), 1, fd);

        for ( int layer_i = 0; ok && layer_i < num_layers; ++layer_i ) {
            i32 len = 0;
            READ(&len, sizeof(i32), 1, fd);

            if ( len > MAX_LAYER_NAME_LEN ) {
                milton_log("Corrupt file. Layer name is too long.\n");
                ok = false;
                goto END;
            }

            if (ok) { milton_new_layer(milton_state); }

            Layer* layer = milton_state->canvas->working_layer;

            READ(layer->name, sizeof(char), (size_t)len, fd);

            READ(&layer->id, sizeof(i32), 1, fd);
            READ(&layer->flags, sizeof(layer->flags), 1, fd);

            if ( ok ) {
                i32 num_strokes = 0;
                READ(&num_strokes, sizeof(i32), 1, fd);

                for ( i32 stroke_i = 0; ok && stroke_i < num_strokes; ++stroke_i ) {
                    Stroke stroke = Stroke{};

                    stroke.id = milton_state->canvas->stroke_id_count++;

                    READ(&stroke.brush, sizeof(Brush), 1, fd);
                    READ(&stroke.num_points, sizeof(i32), 1, fd);

                    if ( stroke.num_points >= STROKE_MAX_POINTS || stroke.num_points <= 0 ) {
                        milton_log("ERROR: File has a stroke with %d points\n",
                                   stroke.num_points);
                        reset(&milton_state->canvas->root_layer->strokes);
                        ok = false;
                        goto END;
                    } else {
                        if ( milton_binary_version >= 4 ) {
                            stroke.points = arena_alloc_array(&canvas->arena, stroke.num_points, v2l);
                            READ(stroke.points, sizeof(v2l), (size_t)stroke.num_points, fd);
                        } else {
                            stroke.points = arena_alloc_array(&canvas->arena, stroke.num_points, v2l);
                            v2i* points_32bit = (v2i*)mlt_calloc((size_t)stroke.num_points, sizeof(v2i), "Persist");

                            READ(points_32bit, sizeof(v2i), (size_t)stroke.num_points, fd);
                            for (int i = 0; i < stroke.num_points; ++i) {
                                stroke.points[i] = VEC2L(points_32bit[i]);
                            }
                        }
                        stroke.pressures = arena_alloc_array(&canvas->arena, stroke.num_points, f32);
                        READ(stroke.pressures, sizeof(f32), (size_t)stroke.num_points, fd);
                        READ(&stroke.layer_id, sizeof(i32), 1, fd);
                        stroke.bounding_rect = bounding_box_for_stroke(&stroke);

                        layer_push_stroke(layer, stroke);
                    }
                }
            }
            if ( milton_binary_version >= 4 ) {
                i64 num_effects = 0;
                READ(&num_effects, sizeof(num_effects), 1, fd);
                if ( num_effects > 0 ) {
                    LayerEffect** e = &layer->effects;
                    for ( i64 i = 0; i < num_effects; ++i ) {
                        mlt_assert(*e == NULL);
                        *e = arena_alloc_elem(&canvas->arena, LayerEffect);
                        READ(&(*e)->type, sizeof((*e)->type), 1, fd);
                        READ(&(*e)->enabled, sizeof((*e)->enabled), 1, fd);
                        switch ((*e)->type) {
                            case LayerEffectType_BLUR: {
                                READ(&(*e)->blur.original_scale, sizeof((*e)->blur.original_scale), 1, fd);
                                READ(&(*e)->blur.kernel_size, sizeof((*e)->blur.kernel_size), 1, fd);
                            } break;
                        }
                        e = &(*e)->next;
                    }
                }
            }
        }
        milton_state->view->working_layer_id = saved_working_layer_id;
        READ(&milton_state->gui->picker.data, sizeof(PickerData), 1, fd);

        // Buttons
    {
        i32 button_count = 0;
        gui = milton_state->gui;
        btn = gui->picker.color_buttons;

        READ(&button_count, sizeof(i32), 1, fd);
        for ( i32 i = 0;
              btn!=NULL && i < button_count;
              ++i, btn=btn->next ) {
            READ(&btn->rgba, sizeof(v4f), 1, fd);
        }
        }

        // Brush
        if ( milton_binary_version >= 2 ) {
            // PEN, ERASER
            READ(&milton_state->brushes, sizeof(Brush), BrushEnum_COUNT, fd);
            // Sizes
            READ(&milton_state->brush_sizes, sizeof(i32), BrushEnum_COUNT, fd);
        }

        history_count = 0;
        READ(&history_count, sizeof(history_count), 1, fd);
        reset(&milton_state->canvas->history);
        reserve(&milton_state->canvas->history, history_count);
        READ(milton_state->canvas->history.data, sizeof(*milton_state->canvas->history.data), (size_t)history_count, fd);
        milton_state->canvas->history.count = history_count;

        // MLT 3
        // Layer alpha
        if ( milton_binary_version >= 3 ) {
            Layer* l = milton_state->canvas->root_layer;
            for ( i64 i = 0; ok && i < num_layers; ++i ) {
                mlt_assert(l != NULL);
                READ(&l->alpha, sizeof(l->alpha), 1, fd);
                l = l->next;
            }
        } else {
            for ( Layer* l = milton_state->canvas->root_layer; l != NULL; l = l->next ) {
                l->alpha = 1.0f;
            }
        }

        err = fclose(fd);
        if ( err != 0 ) {
            ok = false;
        }

END:
        // Finished loading
        if ( !ok ) {
            if ( !handled ) {
                platform_dialog("Tried to load a corrupt Milton file or there was an error reading from disk.", "Error");
            }
            milton_reset_canvas_and_set_default(milton_state);
        } else {
            i32 id = milton_state->view->working_layer_id;
            {  // Use working_layer_id to make working_layer point to the correct thing
                Layer* layer = milton_state->canvas->root_layer;
                while ( layer ) {
                    if ( layer->id == id ) {
                        milton_state->canvas->working_layer = layer;
                        break;
                    }
                    layer = layer->next;
                }
            }
            milton_state->canvas->layer_guid = layer_guid;

            // Update GPU
            milton_set_background_color(milton_state, milton_state->view->background_color);
            gpu_update_picker(milton_state->render_data, &milton_state->gui->picker);
        }
    } else {
        milton_log("Could not open file!");
        milton_reset_canvas_and_set_default(milton_state);
    }
#undef READ
}

void
milton_save(MiltonState* milton_state)
{
    // Declaring variables here to silence compiler warnings about GOTO jumping declarations.
    i32 history_count = 0;
    u32 milton_binary_version = 0;
    i32 num_layers = 0;
    milton_state->flags |= MiltonStateFlags_LAST_SAVE_FAILED;  // Assume failure. Remove flag on success.

    int pid = (int)getpid();
    PATH_CHAR tmp_fname[MAX_PATH] = {0};
    PATH_SNPRINTF(tmp_fname, MAX_PATH, TO_PATH_STR("milton_tmp.%d.mlt"), pid);

    platform_fname_at_config(tmp_fname, MAX_PATH);

    FILE* fd = platform_fopen(tmp_fname, TO_PATH_STR("wb"));

    b32 ok = true;

    if ( fd ) {
#define WRITE(address, sz, num, fd) do { ok = fwrite_checked(address, sz, num, fd); if (!ok) { goto END; }  } while(0)
        u32 milton_magic = MILTON_MAGIC_NUMBER;

        WRITE(&milton_magic, sizeof(u32), 1, fd);

        milton_binary_version = milton_state->mlt_binary_version;

        WRITE(&milton_binary_version, sizeof(u32), 1, fd);
        WRITE(milton_state->view, sizeof(CanvasView), 1, fd);

        num_layers = number_of_layers(milton_state->canvas->root_layer);
        WRITE(&num_layers, sizeof(i32), 1, fd);
        WRITE(&milton_state->canvas->layer_guid, sizeof(i32), 1, fd);

        for ( Layer* layer = milton_state->canvas->root_layer; layer; layer=layer->next  ) {
            if ( layer->strokes.count > INT_MAX ) {
                milton_die_gracefully("FATAL. Number of strokes in layer greater than can be stored in file format. ");
            }
            i32 num_strokes = (i32)layer->strokes.count;
            char* name = layer->name;
            i32 len = (i32)(strlen(name) + 1);
            WRITE(&len, sizeof(i32), 1, fd);
            WRITE(name, sizeof(char), (size_t)len, fd);
            WRITE(&layer->id, sizeof(i32), 1, fd);
            WRITE(&layer->flags, sizeof(layer->flags), 1, fd);
            WRITE(&num_strokes, sizeof(i32), 1, fd);
            if ( ok ) {
                for ( i32 stroke_i = 0; ok && stroke_i < num_strokes; ++stroke_i ) {
                    Stroke* stroke = get(&layer->strokes, stroke_i);
                    mlt_assert(stroke->num_points > 0);
                    WRITE(&stroke->brush, sizeof(Brush), 1, fd);
                    WRITE(&stroke->num_points, sizeof(i32), 1, fd);
                    WRITE(stroke->points, sizeof(v2l), (size_t)stroke->num_points, fd);
                    WRITE(stroke->pressures, sizeof(f32), (size_t)stroke->num_points, fd);
                    WRITE(&stroke->layer_id, sizeof(i32), 1, fd);
                    if ( !ok ) {
                        break;
                    }
                }
            } else {
                ok = false;
            }
            {
                i64 num_effects = 0;
                for ( LayerEffect* e = layer->effects; e != NULL; e = e->next ) {
                    ++num_effects;
                }
                WRITE(&num_effects, sizeof(num_effects), 1, fd);
                for ( LayerEffect* e = layer->effects; e != NULL; e = e->next ) {
                    WRITE(&e->type, sizeof(e->type), 1, fd);
                    WRITE(&e->enabled, sizeof(e->enabled), 1, fd);
                    switch (e->type) {
                        case LayerEffectType_BLUR: {
                            WRITE(&e->blur.original_scale, sizeof(e->blur.original_scale), 1, fd);
                            WRITE(&e->blur.kernel_size, sizeof(e->blur.kernel_size), 1, fd);
                        } break;
                    }
                }
            }
        }

        WRITE(&milton_state->gui->picker.data, sizeof(PickerData), 1, fd);

        // Buttons
        if ( ok ) {
            i32 button_count = 0;
            MiltonGui* gui = milton_state->gui;
            // Count buttons
            for (ColorButton* b = gui->picker.color_buttons; b!= NULL; b = b->next, button_count++) { }
            // Write
            WRITE(&button_count, sizeof(i32), 1, fd);
            if ( ok ) {
                for ( ColorButton* b = gui->picker.color_buttons;
                      ok && b!= NULL;
                      b = b->next ) {
                    WRITE(&b->rgba, sizeof(v4f), 1, fd);
                }
            }
        }

        // Brush
        if ( milton_binary_version >= 2 ) {
            // PEN, ERASER
            WRITE(&milton_state->brushes, sizeof(Brush), BrushEnum_COUNT, fd);
            // Sizes
            WRITE(&milton_state->brush_sizes, sizeof(i32), BrushEnum_COUNT, fd);
        }

        history_count = (i32)milton_state->canvas->history.count;
        if ( milton_state->canvas->history.count > INT_MAX ) {
            history_count = 0;
        }
        WRITE(&history_count, sizeof(history_count), 1, fd);
        WRITE(milton_state->canvas->history.data, sizeof(*milton_state->canvas->history.data), (size_t)history_count, fd);

        // MLT 3
        // Layer alpha
        if ( milton_binary_version >= 3 ) {
            Layer* l = milton_state->canvas->root_layer;
            for ( i64 i = 0; ok && i < num_layers; ++i ) {
                mlt_assert(l);
                WRITE(&l->alpha, sizeof(l->alpha), 1, fd);
                l = l->next;
            }
        }
END:
        int file_error = ferror(fd);
        if ( file_error == 0 ) {
            int close_ret = fclose(fd);
            if ( close_ret == 0 ) {
                ok = platform_move_file(tmp_fname, milton_state->mlt_file_path);
                if ( ok ) {
                    //  \o/
                    milton_save_postlude(milton_state);
                }
                else {
                    milton_log("Could not move file. Moving on. Avoiding this save.\n");
                    milton_state->flags |= MiltonStateFlags_MOVE_FILE_FAILED;
                }
            }
            else {
                milton_log("File error when closing handle. Error code %d. \n", close_ret);
            }
        }
        else {
            milton_log("File IO error. Error code %d. \n", file_error);
        }

    }
    else {
        // TODO: Fix this on macos
        milton_die_gracefully("Could not create file for saving! ");
        return;
    }
#undef WRITE
}

PATH_CHAR*
milton_get_last_canvas_fname()
{
    PATH_CHAR* last_fname = (PATH_CHAR*)mlt_calloc(MAX_PATH, sizeof(PATH_CHAR), "Strings");

    PATH_CHAR full[MAX_PATH] = {};

    PATH_STRCPY(full, TO_PATH_STR("saved_path"));
    platform_fname_at_config(full, MAX_PATH);
    FILE* fd = platform_fopen(full, TO_PATH_STR("rb+"));

    if ( fd ) {
        u64 len = 0;
        fread(&len, sizeof(len), 1, fd);
        if ( len < MAX_PATH ) {
            fread(last_fname, sizeof(PATH_CHAR), len, fd);
            // If the read fails, or if the file doesn't exist, milton_load
            // will fail gracefully and load a default canvas.
            fclose(fd);
        }
    } else {
        mlt_free(last_fname, "Strings");
    }

    return last_fname;

}

void
milton_set_last_canvas_fname(PATH_CHAR* last_fname)
{
    //PATH_CHAR* full = (PATH_CHAR*)mlt_calloc(MAX_PATH, sizeof(char));
    //wcscpy(full, "last_canvas_fname");
    PATH_CHAR full[MAX_PATH] = { TO_PATH_STR("saved_path") };
    platform_fname_at_config(full, MAX_PATH);
    FILE* fd = platform_fopen(full, TO_PATH_STR("wb"));
    if ( fd ) {
        u64 len = PATH_STRLEN(last_fname)+1;
        fwrite(&len, sizeof(len), 1, fd);
        fwrite(last_fname, sizeof(*last_fname), len, fd);
        fclose(fd);
    }
}

// Called by stb_image
static void
write_func(void* context, void* data, int size)
{
    FILE* fd = *(FILE**)context;

    if ( fd ) {
        size_t written = fwrite(data, (size_t)size, 1, fd);
        if ( written != 1 ) {
            fclose(fd);
            *(FILE**)context = NULL;
        }
    }
}

void
milton_save_buffer_to_file(PATH_CHAR* fname, u8* buffer, i32 w, i32 h)
{
    int len = 0;
    {
        size_t sz = PATH_STRLEN(fname);
        if ( sz > ((1u << 31) -1) ) {
            milton_die_gracefully("A really, really long file name. This shouldn't happen.");
        }
        len = (int)sz;
    }
    size_t ext_sz = ( len+1 ) * sizeof(PATH_CHAR);
    PATH_CHAR* fname_copy = (PATH_CHAR*)mlt_calloc(ext_sz, 1, "Strings");
    fname_copy[0] = '\0';
    PATH_STRCPY(fname_copy, fname);

    // NOTE: This should work with unicode.
    int ext_len = 0;
    PATH_CHAR* ext = fname_copy + len;
    b32 found = false;
    {
        int safety = len;
        while ( *--ext != '.' ) {
            if( safety-- == 0 ) {
                break;
            }
        }
        if ( safety > 0 ) {
            found = true;
            ext_len = len - safety;
            ++ext;
        }
    }

    if ( found ) {
        for ( int i = 0; i < ext_len; ++i ) {
            PATH_CHAR c = ext[i];
            ext[i] = PATH_TOLOWER(c);
        }

        FILE* fd = NULL;

        fd = platform_fopen(fname, TO_PATH_STR("wb"));

        if ( fd ) {
            if ( !PATH_STRCMP(ext, TO_PATH_STR("png")) ) {
                stbi_write_png_to_func(write_func, &fd, w, h, 4, buffer, 0);
            }
            else if ( !PATH_STRCMP(ext, TO_PATH_STR("jpg")) || !PATH_STRCMP(ext, TO_PATH_STR("jpeg")) ) {
                tje_encode_with_func(write_func, &fd, 3, w, h, 4, buffer);
            }
            else {
                platform_dialog("File extension not handled by Milton\n", "Info");
            }

            // !! fd might have been set to NULL if write_func failed.
            if ( fd ) {
                if ( ferror(fd) ) {
                    platform_dialog("Unknown error when writing to file :(", "Unknown error");
                }
                else {
                    platform_dialog("Image exported successfully!", "Success");
                }
                fclose(fd);
            }
            else {
                platform_dialog("File created, but there was an error writing to it.", "Error");
            }
        }
        else {
            platform_dialog ( "Could not open file", "Error" );
        }
    }
    else {
        platform_dialog("File name missing extension!\n", "Error");
    }
    mlt_free(fname_copy, "Strings");
}

void
milton_prefs_load(PlatformPrefs* prefs)
{
    PATH_CHAR fname[MAX_PATH] = TO_PATH_STR("PREFS.milton_prefs");
    platform_fname_at_config(fname, MAX_PATH);

    FILE* fd = platform_fopen(fname, TO_PATH_STR("rb"));
    if ( fd ) {
        if ( !ferror(fd) ) {
            fread(&prefs->width, sizeof(i32), 1, fd);
            fread(&prefs->height, sizeof(i32), 1, fd);
        }
        else {
            milton_log("Error writing to prefs file...\n");
        }
        fclose(fd);
    }
    else {
        milton_log("Could not open file for writing prefs\n");
    }
}

void
milton_prefs_save(PlatformPrefs* prefs)
{
    PATH_CHAR fname[MAX_PATH] = TO_PATH_STR("PREFS.milton_prefs");
    platform_fname_at_config(fname, MAX_PATH);
    FILE* fd = platform_fopen(fname, TO_PATH_STR("wb"));
    if ( fd ) {
        if ( !ferror(fd) ) {
            fwrite(&prefs->width, sizeof(i32), 1, fd);
            fwrite(&prefs->height, sizeof(i32), 1, fd);
        }
        else {
            milton_log( "Error writing to profs file...\n" );
        }
        fclose(fd);
    }
    else {
        milton_log("Could not open file for writing prefs :(\n");
    }
}
