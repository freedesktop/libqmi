/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * libqmi-glib -- GLib/GIO based library to control QMI devices
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 * Copyright (C) 2021 Aleksander Morgado <aleksander@aleksander.es>
 */

#include <limits.h>
#include <stdlib.h>

#include "qmi-device.h"
#include "qmi-error-types.h"
#include "qmi-errors.h"
#include "qmi-helpers.h"
#include "qmi-net-port-manager-qmiwwan.h"

G_DEFINE_TYPE (QmiNetPortManagerQmiwwan, qmi_net_port_manager_qmiwwan, QMI_TYPE_NET_PORT_MANAGER)

struct _QmiNetPortManagerQmiwwanPrivate {
    gchar *iface;
    gchar *sysfs_path;
    GFile *sysfs_file;
    gchar *add_mux_sysfs_path;
    gchar *del_mux_sysfs_path;

    /* We don't allow running link operations in parallel, because the qmi_wwan
     * add_mux/del_mux may be a bit racy. The races may already happen if there
     * are additional programs trying to do the same, but that's something we'll
     * try to live with. */
    gboolean  running;
    GList    *pending_tasks;

    /* mux id tracking table */
    GHashTable *mux_id_map;
};

/*****************************************************************************/
/* The qmap/mux_id attribute was introduced in a newer kernel version. If
 * we don't have this info, try to keep the track of what iface applies to
 * what mux id manually here.
 *
 * Not perfect, but works if the MM doesn't crash and loses the info.
 * This legacy logic won't make any sense on plain qmicli operations, though.
 */

static gboolean
track_mux_id (QmiNetPortManagerQmiwwan  *self,
              const gchar               *link_iface,
              const gchar               *mux_id,
              GError                   **error)
{
    if (g_hash_table_lookup (self->priv->mux_id_map, link_iface)) {
        g_set_error (error, QMI_CORE_ERROR, QMI_CORE_ERROR_FAILED, "Already exists");
        return FALSE;
    }

    g_hash_table_insert (self->priv->mux_id_map,
                         g_strdup (link_iface),
                         g_strdup (mux_id));
    return TRUE;
}

static gboolean
untrack_mux_id (QmiNetPortManagerQmiwwan  *self,
                const gchar               *link_iface,
                GError                   **error)
{
    if (!g_hash_table_remove (self->priv->mux_id_map, link_iface)) {
        g_set_error (error, QMI_CORE_ERROR, QMI_CORE_ERROR_FAILED, "Not found");
        return FALSE;
    }
    return TRUE;
}

static const gchar *
get_tracked_mux_id (QmiNetPortManagerQmiwwan  *self,
                    const gchar               *link_iface,
                    GError                   **error)
{
    const gchar *found;

    found = g_hash_table_lookup (self->priv->mux_id_map, link_iface);
    if (!found) {
        g_set_error (error, QMI_CORE_ERROR, QMI_CORE_ERROR_FAILED, "Not found");
        return NULL;
    }
    return found;
}

/*****************************************************************************/

static gchar *
read_link_mux_id (const gchar  *link_iface,
                  GError      **error)
{
    g_autofree gchar *link_mux_id_sysfs_path = NULL;
    g_autofree gchar *link_mux_id = NULL;

    /* mux id expected as an hex integer between 0x01 and 0xfe */
    link_mux_id = g_malloc0 (5);
    link_mux_id_sysfs_path = g_strdup_printf ("/sys/class/net/%s/qmap/mux_id", link_iface);

    if (!qmi_helpers_read_sysfs_file (link_mux_id_sysfs_path, link_mux_id, 4, error))
        return NULL;

    return g_steal_pointer (&link_mux_id);
}

static gboolean
lookup_mux_id_in_links (GPtrArray    *links,
                        const gchar  *mux_id,
                        gchar       **out_link,
                        GError      **error)
{
    guint i;

    for (i = 0; links && i < links->len; i++) {
        const gchar      *link_iface;
        g_autofree gchar *link_mux_id = NULL;

        link_iface = g_ptr_array_index (links, i);
        link_mux_id = read_link_mux_id (link_iface, error);
        if (!link_mux_id)
            return FALSE;

        if (g_strcmp0 (mux_id, link_mux_id) == 0) {
            *out_link = g_strdup (link_iface);
            return TRUE;
        }
    }

    *out_link = NULL;
    return TRUE;
}

static gchar *
lookup_first_new_link (GPtrArray *links_before,
                       GPtrArray *links_after)
{
    guint i;

    if (!links_after)
        return NULL;

    for (i = 0; i < links_after->len; i++) {
        const gchar *link_iface;

        link_iface = g_ptr_array_index (links_after, i);

        if (!links_before || !g_ptr_array_find_with_equal_func (links_before, link_iface, g_str_equal, NULL))
            return g_strdup (link_iface);
    }
    return NULL;
}

/*****************************************************************************/

static gint
cmpuint (const guint *a,
         const guint *b)
{
    return ((*a > *b) ? 1 : ((*b > *a) ? -1 : 0));
}

static guint
get_first_free_mux_id (QmiNetPortManagerQmiwwan  *self,
                       GPtrArray                 *links,
                       GError                   **error)
{
    guint              i;
    g_autoptr(GArray)  existing_mux_ids = NULL;
    guint              next_mux_id;
    static const guint max_mux_id_upper_threshold = QMI_DEVICE_MUX_ID_MAX + 1;

    if (!links)
        return QMI_DEVICE_MUX_ID_MIN;

    existing_mux_ids = g_array_new (FALSE, FALSE, sizeof (guint));

    for (i = 0; i < links->len; i++) {
        const gchar      *link_iface;
        g_autofree gchar *link_mux_id = NULL;
        gulong            link_mux_id_num;

        link_iface = g_ptr_array_index (links, i);
        link_mux_id = read_link_mux_id (link_iface, NULL);
        if (!link_mux_id) {
            const gchar *tracked_link_mux_id;

            g_debug ("Couldn't read mux id from sysfs for link '%s': unsupported by driver", link_iface);
            /* fallback to use our internal tracking table... far from perfect */
            tracked_link_mux_id = get_tracked_mux_id (self, link_iface, NULL);
            if (!tracked_link_mux_id) {
                g_set_error (error, QMI_CORE_ERROR, QMI_CORE_ERROR_UNSUPPORTED,
                             "Couldn't get tracked mux id for link '%s'", link_iface);
                return QMI_DEVICE_MUX_ID_UNBOUND;
            }
            link_mux_id_num = strtoul (tracked_link_mux_id, NULL, 16);
        } else
            link_mux_id_num = strtoul (link_mux_id, NULL, 16);

        if (!link_mux_id_num) {
            g_set_error (error, QMI_CORE_ERROR, QMI_CORE_ERROR_FAILED,
                         "Couldn't parse mux id '%s'", link_mux_id);
            return QMI_DEVICE_MUX_ID_UNBOUND;
        }

        g_array_append_val (existing_mux_ids, link_mux_id_num);
    }

    /* add upper level threshold, so that if we end up out of the loop
     * below, it means we have exhausted all mux ids */
    g_array_append_val (existing_mux_ids, max_mux_id_upper_threshold);
    g_array_sort (existing_mux_ids, (GCompareFunc)cmpuint);

    for (next_mux_id = QMI_DEVICE_MUX_ID_MIN, i = 0; i < existing_mux_ids->len; next_mux_id++, i++) {
        guint existing;

        existing = g_array_index (existing_mux_ids, guint, i);
        if (next_mux_id < existing)
            return next_mux_id;
        g_assert_cmpuint (next_mux_id, ==, existing);
    }

    g_set_error (error, QMI_CORE_ERROR, QMI_CORE_ERROR_FAILED, "No mux ids left");
    return QMI_DEVICE_MUX_ID_UNBOUND;
}

/*****************************************************************************/

#define LINK_OPERATION_TIMEOUT_STEP_MS 250

typedef enum {
    LINK_OPERATION_TYPE_ADD,
    LINK_OPERATION_TYPE_DEL,
} LinkOperationType;

typedef struct {
    LinkOperationType  type;
    guint              timeout_ms;
    guint              timeout_ms_elapsed;
    GSource           *timeout_source;
    GPtrArray         *links_before;
    gchar             *mux_id;
    gchar             *link_iface;
} LinkOperationContext;

static void
link_operation_context_free (LinkOperationContext *ctx)
{
    g_free (ctx->mux_id);
    g_free (ctx->link_iface);
    if (ctx->links_before)
        g_ptr_array_unref (ctx->links_before);
    if (ctx->timeout_source) {
        g_source_destroy (ctx->timeout_source);
        g_source_unref (ctx->timeout_source);
    }
    g_slice_free (LinkOperationContext, ctx);
}

static void run_add_link (GTask *task);
static void run_del_link (GTask *task);

static void
link_operation_completed (QmiNetPortManagerQmiwwan *self)
{
    LinkOperationContext *ctx;
    GTask                *task;

    g_assert (self->priv->running);
    self->priv->running = FALSE;

    if (!self->priv->pending_tasks)
        return;

    task = self->priv->pending_tasks->data;
    self->priv->pending_tasks = g_list_delete_link (self->priv->pending_tasks, self->priv->pending_tasks);
    g_assert (task);

    self->priv->running = TRUE;

    ctx = g_task_get_task_data (task);
    if (ctx->type == LINK_OPERATION_TYPE_ADD)
        run_add_link (task);
    else if (ctx->type == LINK_OPERATION_TYPE_DEL)
        run_del_link (task);
    else
        g_assert_not_reached ();
}

static GTask *
link_operation_new (QmiNetPortManagerQmiwwan *self,
                    const gchar              *base_ifname,
                    guint                     timeout,
                    GCancellable             *cancellable,
                    GAsyncReadyCallback       callback,
                    gpointer                  user_data)
{
    GTask                *task;
    LinkOperationContext *ctx;

    task = g_task_new (self, cancellable, callback, user_data);

    /* validate base ifname before doing anything else */
    if (base_ifname && !g_str_equal (base_ifname, self->priv->iface)) {
        g_task_return_new_error (task, QMI_CORE_ERROR, QMI_CORE_ERROR_INVALID_ARGS,
                                 "Invalid base interface given: '%s' (must be '%s')",
                                 base_ifname, self->priv->iface);
        g_object_unref (task);
        return NULL;
    }

    /* Setup a completion signal handler so that we process the pending tasks
     * list once a task has been completed. The self pointer is guaranteed to
     * be valid because the task holds a full reference. */
    g_signal_connect_swapped (task,
                              "notify::completed",
                              G_CALLBACK (link_operation_completed),
                              self);

    ctx = g_slice_new0 (LinkOperationContext);
    ctx->type = base_ifname ? LINK_OPERATION_TYPE_ADD : LINK_OPERATION_TYPE_DEL;;
    ctx->timeout_ms = timeout * 1000;

    g_task_set_task_data (task, ctx, (GDestroyNotify)link_operation_context_free);

    return task;
}

/*****************************************************************************/

static gchar *
net_port_manager_add_link_finish (QmiNetPortManager  *self,
                                  guint              *mux_id,
                                  GAsyncResult       *res,
                                  GError            **error)
{
    gchar *link_name;

    link_name = g_task_propagate_pointer (G_TASK (res), error);
    if (!link_name)
        return NULL;

    if (mux_id) {
        LinkOperationContext *ctx;

        ctx = g_task_get_task_data (G_TASK (res));
        *mux_id = strtoul (ctx->mux_id, NULL, 16);
    }

    return link_name;
}

static gboolean
add_link_check_timeout (GTask *task)
{
    QmiNetPortManagerQmiwwan *self;
    LinkOperationContext     *ctx;
    GError                   *error = NULL;
    g_autoptr(GPtrArray)      links_after = NULL;
    g_autofree gchar         *link_name = NULL;

    if (g_task_return_error_if_cancelled (task)) {
        g_object_unref (task);
        return G_SOURCE_REMOVE;
    }

    self = g_task_get_source_object (task);
    ctx  = g_task_get_task_data (task);

    if (!qmi_helpers_list_links (self->priv->sysfs_file,
                                 g_task_get_cancellable (task),
                                 ctx->links_before,
                                 &links_after,
                                 &error)) {
        g_prefix_error (&error, "Couldn't enumerate files in the sysfs directory: ");
        g_task_return_error (task, error);
        g_object_unref (task);
        return G_SOURCE_REMOVE;
    }

    if (!lookup_mux_id_in_links (links_after, ctx->mux_id, &link_name, &error)) {
        g_debug ("Couldn't find mux_id in network link: %s", error->message);
        g_clear_error (&error);

        /* Now, assume this is because the mux_id attribute was added in a newer
         * kernel. As a fallback, we'll try to detect the first new link listed,
         * even if this is definitely very racy. */
        link_name = lookup_first_new_link (ctx->links_before, links_after);
        if (link_name)
            g_debug ("Found first new link '%s' (unknown mux id)", link_name);
    } else
        g_debug ("Found link '%s' associated to mux id '%s'", link_name, ctx->mux_id);

    if (link_name) {
        if (!track_mux_id (self, link_name, ctx->mux_id, &error)) {
            g_warning ("Couldn't track mux id: %s", error->message);
            g_clear_error (&error);
        }
        g_task_return_pointer (task, g_steal_pointer (&link_name), g_free);
        g_object_unref (task);
        return G_SOURCE_REMOVE;
    }

    g_debug ("Link not yet found, rescheduling...");
    ctx->timeout_ms_elapsed += LINK_OPERATION_TIMEOUT_STEP_MS;
    if (ctx->timeout_ms_elapsed > ctx->timeout_ms) {
        g_task_return_new_error (task, QMI_CORE_ERROR, QMI_CORE_ERROR_TIMEOUT,
                                 "No new link detected for mux id %s", ctx->mux_id);
        g_object_unref (task);
        return G_SOURCE_REMOVE;
    }

    /* retry */
    return G_SOURCE_CONTINUE;
}

static void
run_add_link (GTask *task)
{
    QmiNetPortManagerQmiwwan *self;
    LinkOperationContext     *ctx;
    GError                   *error = NULL;

    g_debug ("Running add link operation...");

    self = g_task_get_source_object (task);
    ctx = g_task_get_task_data (task);

    if (!qmi_helpers_list_links (self->priv->sysfs_file,
                                 g_task_get_cancellable (task),
                                 NULL,
                                 &ctx->links_before,
                                 &error)) {
        g_prefix_error (&error, "Couldn't enumerate files in the sysfs directory: ");
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    if (!ctx->mux_id) {
        guint new_mux_id;

        new_mux_id = get_first_free_mux_id (self, ctx->links_before, &error);
        if (new_mux_id == QMI_DEVICE_MUX_ID_UNBOUND) {
            g_prefix_error (&error, "Couldn't add create link with automatic mux id: ");
            g_task_return_error (task, error);
            g_object_unref (task);
            return;
        }
        g_debug ("Using mux id %u", new_mux_id);
        ctx->mux_id = g_strdup_printf ("0x%02x", new_mux_id);
    }

    if (!qmi_helpers_write_sysfs_file (self->priv->add_mux_sysfs_path, ctx->mux_id, &error)) {
        g_prefix_error (&error, "Couldn't add create link with mux id %s: ", ctx->mux_id);
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    ctx->timeout_source = g_timeout_source_new (LINK_OPERATION_TIMEOUT_STEP_MS);
    g_source_set_callback (ctx->timeout_source,
                           (GSourceFunc) add_link_check_timeout,
                           task,
                           NULL);
    g_source_attach (ctx->timeout_source, g_main_context_get_thread_default ());
}

static void
net_port_manager_add_link (QmiNetPortManager   *_self,
                           guint                mux_id,
                           const gchar         *base_ifname,
                           const gchar         *ifname_prefix,
                           guint                timeout,
                           GCancellable        *cancellable,
                           GAsyncReadyCallback  callback,
                           gpointer             user_data)
{
    QmiNetPortManagerQmiwwan *self = QMI_NET_PORT_MANAGER_QMIWWAN (_self);
    GTask                    *task;
    LinkOperationContext     *ctx;

    g_debug ("Net port manager based on qmi_wwan ignores the ifname prefix '%s'", ifname_prefix);

    task = link_operation_new (QMI_NET_PORT_MANAGER_QMIWWAN (self),
                               base_ifname,
                               timeout,
                               cancellable,
                               callback,
                               user_data);
    if (!task)
        return;

    ctx = g_task_get_task_data (task);

    if (mux_id != QMI_DEVICE_MUX_ID_AUTOMATIC)
        ctx->mux_id = g_strdup_printf ("0x%02x", mux_id);

    /* If there is another task running, queue the new one */
    if (self->priv->running) {
        g_debug ("Queueing add link operation...");
        self->priv->pending_tasks = g_list_append (self->priv->pending_tasks, task);
        return;
    }
    self->priv->running = TRUE;

    run_add_link (task);
}

/*****************************************************************************/

static gboolean
net_port_manager_del_link_finish (QmiNetPortManager  *self,
                                  GAsyncResult       *res,
                                  GError            **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static gboolean
del_link_check_timeout (GTask *task)
{
    QmiNetPortManagerQmiwwan *self;
    LinkOperationContext     *ctx;
    GError                   *error = NULL;
    g_autoptr(GPtrArray)      links_after = NULL;

    if (g_task_return_error_if_cancelled (task)) {
        g_object_unref (task);
        return G_SOURCE_REMOVE;
    }

    self = g_task_get_source_object (task);
    ctx  = g_task_get_task_data (task);

    if (!qmi_helpers_list_links (self->priv->sysfs_file,
                                 g_task_get_cancellable (task),
                                 ctx->links_before,
                                 &links_after,
                                 &error)) {
        g_prefix_error (&error, "Couldn't enumerate files in the sysfs directory: ");
        g_task_return_error (task, error);
        g_object_unref (task);
        return G_SOURCE_REMOVE;
    }

    if (!links_after || !g_ptr_array_find_with_equal_func (links_after, ctx->link_iface, g_str_equal, NULL)) {
        if (!untrack_mux_id (self, ctx->link_iface, &error)) {
            g_debug ("couldn't untrack mux id: %s", error->message);
            g_clear_error (&error);
        }
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return G_SOURCE_REMOVE;
    }

    ctx->timeout_ms_elapsed += LINK_OPERATION_TIMEOUT_STEP_MS;
    if (ctx->timeout_ms_elapsed > ctx->timeout_ms) {
        g_task_return_new_error (task, QMI_CORE_ERROR, QMI_CORE_ERROR_TIMEOUT,
                                 "link '%s' still detected", ctx->link_iface);
        g_object_unref (task);
        return G_SOURCE_REMOVE;
    }

    /* retry */
    return G_SOURCE_CONTINUE;
}

static void
run_del_link (GTask *task)
{
    QmiNetPortManagerQmiwwan *self;
    LinkOperationContext     *ctx;
    GError                   *error = NULL;

    self = g_task_get_source_object (task);
    ctx = g_task_get_task_data (task);

    g_debug ("Running del link operation...");

    if (!qmi_helpers_list_links (self->priv->sysfs_file,
                                 g_task_get_cancellable (task),
                                 NULL,
                                 &ctx->links_before,
                                 &error)) {
        g_prefix_error (&error, "Couldn't enumerate files in the sysfs directory: ");
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    if (!ctx->links_before || !g_ptr_array_find_with_equal_func (ctx->links_before, ctx->link_iface, g_str_equal, NULL)) {
        g_task_return_new_error (task, QMI_CORE_ERROR, QMI_CORE_ERROR_INVALID_ARGS,
                                 "Cannot delete link '%s': interface not found",
                                 ctx->link_iface);
        g_object_unref (task);
        return;
    }

    /* Try to guess mux id if not given as input */
    if (!ctx->mux_id) {
        ctx->mux_id = read_link_mux_id (ctx->link_iface, &error);
        if (!ctx->mux_id) {
            const gchar *mux_id;

            g_debug ("Couldn't read mux id from sysfs: %s", error->message);
            g_clear_error (&error);

            mux_id = get_tracked_mux_id (self, ctx->link_iface, &error);
            if (!mux_id) {
                g_debug ("Couldn't get tracked mux id: %s", error->message);
                g_clear_error (&error);

                g_task_return_new_error (task, QMI_CORE_ERROR, QMI_CORE_ERROR_INVALID_ARGS,
                                         "Cannot delete link '%s': unknown mux id",
                                         ctx->link_iface);
                g_object_unref (task);
                return;
            }
        }
    }

    if (!qmi_helpers_write_sysfs_file (self->priv->del_mux_sysfs_path, ctx->mux_id, &error)) {
        g_prefix_error (&error, "Couldn't delete link with mux id %s: ", ctx->mux_id);
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    ctx->timeout_source = g_timeout_source_new (LINK_OPERATION_TIMEOUT_STEP_MS);
    g_source_set_callback (ctx->timeout_source,
                           (GSourceFunc) del_link_check_timeout,
                           task,
                           NULL);
    g_source_attach (ctx->timeout_source, g_main_context_get_thread_default ());
}

static void
net_port_manager_del_link (QmiNetPortManager   *_self,
                           const gchar         *ifname,
                           guint                mux_id,
                           guint                timeout,
                           GCancellable        *cancellable,
                           GAsyncReadyCallback  callback,
                           gpointer             user_data)
{
    QmiNetPortManagerQmiwwan *self = QMI_NET_PORT_MANAGER_QMIWWAN (_self);
    GTask                    *task;
    LinkOperationContext     *ctx;

    task = link_operation_new (QMI_NET_PORT_MANAGER_QMIWWAN (self),
                               NULL, /* no base ifname, it's a delete operation */
                               timeout,
                               cancellable,
                               callback,
                               user_data);
    g_assert (task);

    ctx = g_task_get_task_data (task);
    ctx->link_iface = g_strdup (ifname);
    ctx->mux_id = (mux_id != QMI_DEVICE_MUX_ID_UNBOUND) ? g_strdup_printf ("0x%02x", mux_id) : NULL;

    /* If there is another task running, queue the new one */
    if (self->priv->running) {
        g_debug ("Queueing del link operation...");
        self->priv->pending_tasks = g_list_append (self->priv->pending_tasks, task);
        return;
    }
    self->priv->running = TRUE;

    run_del_link (task);
}

/*****************************************************************************/

QmiNetPortManagerQmiwwan *
qmi_net_port_manager_qmiwwan_new (const gchar  *iface,
                                  GError      **error)
{
    g_autoptr(QmiNetPortManagerQmiwwan) self = NULL;

    self = QMI_NET_PORT_MANAGER_QMIWWAN (g_object_new (QMI_TYPE_NET_PORT_MANAGER_QMIWWAN, NULL));

    self->priv->iface = g_strdup (iface);

    self->priv->sysfs_path = g_strdup_printf ("/sys/class/net/%s", iface);
    self->priv->sysfs_file = g_file_new_for_path (self->priv->sysfs_path);

    self->priv->add_mux_sysfs_path = g_strdup_printf ("%s/qmi/add_mux", self->priv->sysfs_path);
    self->priv->del_mux_sysfs_path = g_strdup_printf ("%s/qmi/del_mux", self->priv->sysfs_path);

    if (!g_file_test (self->priv->add_mux_sysfs_path, G_FILE_TEST_EXISTS) ||
        !g_file_test (self->priv->del_mux_sysfs_path, G_FILE_TEST_EXISTS)) {
        g_set_error (error, QMI_CORE_ERROR, QMI_CORE_ERROR_FAILED,
                     "No support for multiplexing in the interface");
        g_object_unref (self);
        return NULL;
    }

    return g_steal_pointer (&self);
}

/*****************************************************************************/

static void
qmi_net_port_manager_qmiwwan_init (QmiNetPortManagerQmiwwan *self)
{
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
                                              QMI_TYPE_NET_PORT_MANAGER_QMIWWAN,
                                              QmiNetPortManagerQmiwwanPrivate);

    self->priv->mux_id_map = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
}

static void
finalize (GObject *object)
{
    QmiNetPortManagerQmiwwan *self = QMI_NET_PORT_MANAGER_QMIWWAN (object);

    g_hash_table_unref (self->priv->mux_id_map);
    g_free (self->priv->iface);
    g_object_unref (self->priv->sysfs_file);
    g_free (self->priv->sysfs_path);
    g_free (self->priv->del_mux_sysfs_path);
    g_free (self->priv->add_mux_sysfs_path);

    G_OBJECT_CLASS (qmi_net_port_manager_qmiwwan_parent_class)->finalize (object);
}

static void
qmi_net_port_manager_qmiwwan_class_init (QmiNetPortManagerQmiwwanClass *klass)
{
    GObjectClass           *object_class = G_OBJECT_CLASS (klass);
    QmiNetPortManagerClass *net_port_manager_class = QMI_NET_PORT_MANAGER_CLASS (klass);

    g_type_class_add_private (object_class, sizeof (QmiNetPortManagerQmiwwanPrivate));

    object_class->finalize = finalize;

    net_port_manager_class->add_link = net_port_manager_add_link;
    net_port_manager_class->add_link_finish = net_port_manager_add_link_finish;
    net_port_manager_class->del_link = net_port_manager_del_link;
    net_port_manager_class->del_link_finish = net_port_manager_del_link_finish;
}
