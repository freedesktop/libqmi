#!/usr/bin/env python
# -*- Mode: python; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
#
# This program is free software; you can redistribute it and/or modify it under
# the terms of the GNU Lesser General Public License as published by the Free
# Software Foundation; either version 2 of the License, or (at your option) any
# later version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more
# details.
#
# You should have received a copy of the GNU Lesser General Public License along
# with this program; if not, write to the Free Software Foundation, Inc., 51
# Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
#
# Copyright (C) 2012 Lanedo GmbH
# Copyright (C) 2012-2017 Aleksander Morgado <aleksander@aleksander.es>
#

import string

from MessageList import MessageList
import utils

"""
The Client class is responsible for providing the QmiClient-based service
specific client GObject.
"""
class Client:

    """
    Constructor
    """
    def __init__(self, objects_dictionary):
        self.name = None
        self.service = None

        # Loop items in the list, looking for the special 'Client' type
        for object_dictionary in objects_dictionary:
            if object_dictionary['type'] == 'Client':
                self.name = object_dictionary['name']
                self.since = object_dictionary['since'] if 'since' in object_dictionary else ''
            elif object_dictionary['type'] == 'Service':
                self.service = object_dictionary['name']

        # We NEED the Client field and the Service field
        if self.name is None:
            raise ValueError('Missing Client field')
        if self.service is None:
            raise ValueError('Missing Service field')

    """
    Emits the generic GObject class implementation
    """
    def __emit_class(self, hfile, cfile, message_list):

        # Check if we'll have indications
        has_indications = False
        for message in message_list.list:
            if message.type == 'Indication':
                has_indications = True
                break

        translations = { 'underscore'                 : utils.build_underscore_name(self.name),
                         'no_prefix_underscore_upper' : utils.build_underscore_name(self.name[4:]).upper(),
                         'camelcase'                  : utils.build_camelcase_name(self.name),
                         'hyphened'                   : utils.build_dashed_name(self.name),
                         'since'                      : self.since,
                         'service'                    : self.service.upper() }

        # Emit class header
        template = (
            '#define QMI_TYPE_${no_prefix_underscore_upper}            (${underscore}_get_type ())\n'
            '#define QMI_${no_prefix_underscore_upper}(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), QMI_TYPE_${no_prefix_underscore_upper}, ${camelcase}))\n'
            '#define QMI_${no_prefix_underscore_upper}_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass),  QMI_TYPE_${no_prefix_underscore_upper}, ${camelcase}Class))\n'
            '#define QMI_IS_${no_prefix_underscore_upper}(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), QMI_TYPE_${no_prefix_underscore_upper}))\n'
            '#define QMI_IS_${no_prefix_underscore_upper}_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass),  QMI_TYPE_${no_prefix_underscore_upper}))\n'
            '#define QMI_${no_prefix_underscore_upper}_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj),  QMI_TYPE_${no_prefix_underscore_upper}, ${camelcase}Class))\n'
            '\n'
            'typedef struct _${camelcase} ${camelcase};\n'
            'typedef struct _${camelcase}Class ${camelcase}Class;\n'
            '\n'
            '/**\n'
            ' * ${camelcase}:\n'
            ' *\n'
            ' * The #${camelcase} structure contains private data and should only be accessed\n'
            ' * using the provided API.\n'
            ' *\n'
            ' * Since: ${since}\n'
            ' */\n'
            'struct _${camelcase} {\n'
            '    /*< private >*/\n'
            '    QmiClient parent;\n'
            '    gpointer priv_unused;\n'
            '};\n'
            '\n'
            'struct _${camelcase}Class {\n'
            '    /*< private >*/\n'
            '    QmiClientClass parent;\n'
            '};\n'
            '\n'
            'GType ${underscore}_get_type (void);\n'
            '\n')
        hfile.write(string.Template(template).substitute(translations))

        # Emit class source. Documentation skipped for the CTL service.
        template = ''
        if self.service != 'CTL':
            template += (
                '\n'
                '/**\n'
                ' * SECTION: ${hyphened}\n'
                ' * @title: ${camelcase}\n'
                ' * @short_description: #QmiClient for the ${service} service.\n'
                ' *\n'
                ' * #QmiClient which handles operations in the ${service} service.\n'
                ' */\n'
                '\n')
        template += (
            'G_DEFINE_TYPE (${camelcase}, ${underscore}, QMI_TYPE_CLIENT);\n')

        if has_indications:
            template += (
                '\n'
                'enum {\n')
            for message in message_list.list:
                if message.type == 'Indication':
                    translations['signal_id'] = utils.build_underscore_uppercase_name(message.name)
                    inner_template = (
                        '    SIGNAL_${signal_id},\n')
                    template += string.Template(inner_template).substitute(translations)
            template += (
                '    SIGNAL_LAST\n'
                '};\n'
                '\n'
                'static guint signals[SIGNAL_LAST] = { 0 };\n')

        template += (
            '\n'
            'static void\n'
            'process_indication (QmiClient *self,\n'
            '                    QmiMessage *message)\n'
            '{\n'
            '    switch (qmi_message_get_message_id (message)) {\n')

        for message in message_list.list:
            if message.type == 'Indication':
                translations['enum_name'] = message.id_enum_name
                translations['message_fullname_underscore'] = utils.build_underscore_name(message.fullname)
                translations['message_name'] = message.name
                translations['signal_id'] = utils.build_underscore_uppercase_name(message.name)
                inner_template = ''
                if message.output is not None and message.output.fields is not None:
                    # At least one field in the indication
                    translations['output_camelcase'] = utils.build_camelcase_name(message.output.fullname)
                    translations['output_underscore'] = utils.build_underscore_name(message.output.fullname)
                    translations['output_underscore'] = utils.build_underscore_name(message.output.fullname)
                    inner_template += (
                        '        case ${enum_name}: {\n'
                        '            ${output_camelcase} *output;\n'
                        '            GError *error = NULL;\n'
                        '\n'
                        '            /* Parse indication */\n'
                        '            output = __${message_fullname_underscore}_indication_parse (message, &error);\n'
                        '            if (!output) {\n'
                        '                g_warning ("Couldn\'t parse \'${message_name}\' indication: %s",\n'
                        '                           error ? error->message : "Unknown error");\n'
                        '                if (error)\n'
                        '                    g_error_free (error);\n'
                        '            } else {\n'
                        '                g_signal_emit (self, signals[SIGNAL_${signal_id}], 0, output);\n'
                        '                ${output_underscore}_unref (output);\n'
                        '            }\n'
                        '            break;\n'
                        '        }\n')
                else:
                    # No output field in the indication
                    inner_template += (
                        '        case ${enum_name}: {\n'
                        '            g_signal_emit (self, signals[SIGNAL_${signal_id}], 0, NULL);\n'
                        '            break;\n'
                        '        }\n')

                template += string.Template(inner_template).substitute(translations)

        template += (
            '        default:\n'
            '            break;\n'
            '    }\n'
            '}\n'
            '\n'
            'static void\n'
            '${underscore}_init (${camelcase} *self)\n'
            '{\n'
            '}\n'
            '\n'
            'static void\n'
            '${underscore}_class_init (${camelcase}Class *klass)\n'
            '{\n'
            '    QmiClientClass *client_class = QMI_CLIENT_CLASS (klass);\n'
            '\n'
            '    client_class->process_indication = process_indication;\n')

        for message in message_list.list:
            if message.type == 'Indication':
                translations['signal_name'] = utils.build_dashed_name(message.name)
                translations['signal_id'] = utils.build_underscore_uppercase_name(message.name)
                translations['message_name'] = message.name
                translations['since'] = message.since
                inner_template = ''
                if message.output is not None and message.output.fields is not None:
                    # At least one field in the indication
                    translations['output_camelcase'] = utils.build_camelcase_name(message.output.fullname)
                    translations['bundle_type'] = 'QMI_TYPE_' + utils.remove_prefix(utils.build_underscore_uppercase_name(message.output.fullname), 'QMI_')
                    translations['service'] = self.service.upper()
                    translations['message_name_dashed'] = message.name.replace(' ', '-')
                    inner_template += (
                        '\n'
                        '    /**\n'
                        '     * ${camelcase}::${signal_name}:\n'
                        '     * @object: A #${camelcase}.\n'
                        '     * @output: A #${output_camelcase}.\n'
                        '     *\n'
                        '     * The ::${signal_name} signal gets emitted when a \'<link linkend=\"libqmi-glib-${service}-${message_name_dashed}-indication.top_of_page\">${message_name}</link>\' indication is received.\n'
                        '     *\n'
                        '     * Since: ${since}\n'
                        '     */\n'
                        '    signals[SIGNAL_${signal_id}] =\n'
                        '        g_signal_new ("${signal_name}",\n'
                        '                      G_OBJECT_CLASS_TYPE (G_OBJECT_CLASS (klass)),\n'
                        '                      G_SIGNAL_RUN_LAST,\n'
                        '                      0,\n'
                        '                      NULL,\n'
                        '                      NULL,\n'
                        '                      NULL,\n'
                        '                      G_TYPE_NONE,\n'
                        '                      1,\n'
                        '                      ${bundle_type});\n')
                else:
                    # No output field in the indication
                    inner_template += (
                        '\n'
                        '    /**\n'
                        '     * ${camelcase}::${signal_name}:\n'
                        '     * @object: A #${camelcase}.\n'
                        '     *\n'
                        '     * The ::${signal_name} signal gets emitted when a \'${message_name}\' indication is received.\n'
                        '     *\n'
                        '     * Since: ${since}\n'
                        '     */\n'
                        '    signals[SIGNAL_${signal_id}] =\n'
                        '        g_signal_new ("${signal_name}",\n'
                        '                      G_OBJECT_CLASS_TYPE (G_OBJECT_CLASS (klass)),\n'
                        '                      G_SIGNAL_RUN_LAST,\n'
                        '                      0,\n'
                        '                      NULL,\n'
                        '                      NULL,\n'
                        '                      NULL,\n'
                        '                      G_TYPE_NONE,\n'
                        '                      0);\n')
                template += string.Template(inner_template).substitute(translations)

        template += (
            '}\n'
            '\n')
        cfile.write(string.Template(template).substitute(translations))


    """
    Emits the async methods for each known request/response
    """
    def __emit_methods(self, hfile, cfile, message_list):
        translations = { 'underscore'        : utils.build_underscore_name(self.name),
                         'camelcase'         : utils.build_camelcase_name (self.name),
                         'service_lowercase' : self.service.lower(),
                         'service_uppercase' : self.service.upper(),
                         'service_camelcase' : string.capwords(self.service) }

        for message in message_list.list:

            if message.type == 'Indication':
                continue

            if message.static:
                continue

            translations['message_name'] = message.name
            translations['message_vendor_id'] = message.vendor
            translations['message_underscore'] = utils.build_underscore_name(message.name)
            translations['message_fullname_underscore'] = utils.build_underscore_name(message.fullname)
            translations['input_camelcase'] = utils.build_camelcase_name(message.input.fullname)
            translations['output_camelcase'] = utils.build_camelcase_name(message.output.fullname)
            translations['input_underscore'] = utils.build_underscore_name(message.input.fullname)
            translations['output_underscore'] = utils.build_underscore_name(message.output.fullname)
            translations['message_since'] = message.since

            if message.input.fields is None:
                translations['input_arg'] = 'gpointer unused'
                translations['input_var'] = 'NULL'
                translations['input_doc'] = 'unused: %NULL. This message doesn\'t have any input bundle.'
            else:
                translations['input_arg'] = translations['input_camelcase'] + ' *input'
                translations['input_var'] = 'input'
                translations['input_doc'] = 'input: a #' + translations['input_camelcase'] + '.'
            template = (
                '\n'
                '/**\n'
                ' * ${underscore}_${message_underscore}:\n'
                ' * @self: a #${camelcase}.\n'
                ' * @${input_doc}\n'
                ' * @timeout: maximum time to wait for the method to complete, in seconds.\n'
                ' * @cancellable: a #GCancellable or %NULL.\n'
                ' * @callback: a #GAsyncReadyCallback to call when the request is satisfied.\n'
                ' * @user_data: user data to pass to @callback.\n'
                ' *\n'
                ' * Asynchronously sends a ${message_name} request to the device.\n'
                ' *\n'
                ' * When the operation is finished, @callback will be invoked in the thread-default main loop of the thread you are calling this method from.\n'
                ' *\n'
                ' * You can then call ${underscore}_${message_underscore}_finish() to get the result of the operation.\n'
                ' *\n'
                ' * Since: ${message_since}\n'
                ' */\n'
                'void ${underscore}_${message_underscore} (\n'
                '    ${camelcase} *self,\n'
                '    ${input_arg},\n'
                '    guint timeout,\n'
                '    GCancellable *cancellable,\n'
                '    GAsyncReadyCallback callback,\n'
                '    gpointer user_data);\n'
                '\n'
                '/**\n'
                ' * ${underscore}_${message_underscore}_finish:\n'
                ' * @self: a #${camelcase}.\n'
                ' * @res: the #GAsyncResult obtained from the #GAsyncReadyCallback passed to ${underscore}_${message_underscore}().\n'
                ' * @error: Return location for error or %NULL.\n'
                ' *\n'
                ' * Finishes an async operation started with ${underscore}_${message_underscore}().\n'
                ' *\n'
                ' * Returns: a #${output_camelcase}, or %NULL if @error is set. The returned value should be freed with ${output_underscore}_unref().\n'
                ' *\n'
                ' * Since: ${message_since}\n'
                ' */\n'
                '${output_camelcase} *${underscore}_${message_underscore}_finish (\n'
                '    ${camelcase} *self,\n'
                '    GAsyncResult *res,\n'
                '    GError **error);\n')
            hfile.write(string.Template(template).substitute(translations))

            template = (
                '\n'
                '${output_camelcase} *\n'
                '${underscore}_${message_underscore}_finish (\n'
                '    ${camelcase} *self,\n'
                '    GAsyncResult *res,\n'
                '    GError **error)\n'
                '{\n'
                '   return g_task_propagate_pointer (G_TASK (res), error);\n'
                '}\n')

            if message.abort:
                template += (
                '\n'
                'static void\n'
                '${message_underscore}_abort_ready (\n'
                '    QmiDevice *device,\n'
                '    GAsyncResult *res)\n'
                '{\n'
                '    GError *error = NULL;\n'
                '    QmiMessage *reply;\n'
                '    QmiMessage${service_camelcase}AbortOutput *output;\n'
                '\n'
                '    reply = qmi_device_command_finish (device, res, &error);\n'
                '    if (reply) {\n'
                '        output = __qmi_message_${service_lowercase}_abort_response_parse (reply, &error);\n'
                '        if (output)\n'
                '            qmi_message_${service_lowercase}_abort_output_unref (output);\n'
                '        qmi_message_unref (reply);\n'
                '    }\n'
                '\n'
                '    if (error) {\n'
                '        g_debug ("Operation to abort \'${message_name}\' failed: %s", error->message);\n'
                '        g_error_free (error);\n'
                '    }\n'
                '}\n')

            template += (
                '\n'
                'static void\n'
                '${message_underscore}_ready (\n'
                '    QmiDevice *device,\n'
                '    GAsyncResult *res,\n'
                '    GTask *task)\n'
                '{\n'
                '    GError *error = NULL;\n'
                '    QmiMessage *reply;\n'
                '    ${output_camelcase} *output;\n'
                '\n'
                '    reply = qmi_device_command_full_finish (device, res, &error);\n'
                '    if (!reply) {\n')

            if message.abort:
                template += (
                    '        if (g_error_matches (error, QMI_CORE_ERROR, QMI_CORE_ERROR_TIMEOUT) ||\n'
                    '            g_error_matches (error, QMI_PROTOCOL_ERROR, QMI_PROTOCOL_ERROR_ABORTED)) {\n'
                    '                QmiMessage *abort;\n'
                    '                GObject *self;\n'
                    '                guint16 transaction_id;\n'
                    '                QmiMessage${service_camelcase}AbortInput *input;\n'
                    '\n'
                    '                self = g_task_get_source_object (task);\n'
                    '                g_assert (self != NULL);\n'
                    '\n'
                    '                transaction_id = (guint16) GPOINTER_TO_UINT (g_task_get_task_data (task));\n'
                    '                g_assert (transaction_id != 0);\n'
                    '\n'
                    '                input = qmi_message_${service_lowercase}_abort_input_new ();\n'
                    '                qmi_message_${service_lowercase}_abort_input_set_transaction_id (\n'
                    '                    input,\n'
                    '                    transaction_id,\n'
                    '                    NULL);\n'
                    '                abort = __qmi_message_${service_lowercase}_abort_request_create (\n'
                    '                            qmi_client_get_next_transaction_id (QMI_CLIENT (self)),\n'
                    '                            qmi_client_get_cid (QMI_CLIENT (self)),\n'
                    '                            input,\n'
                    '                            NULL);\n'
                    '                g_assert (abort != NULL);\n'
                    '                qmi_device_command (device,\n'
                    '                                    abort,\n'
                    '                                    30,\n'
                    '                                    NULL,\n'
                    '                                    (GAsyncReadyCallback)${message_underscore}_abort_ready,\n'
                    '                                    NULL);\n'
                    '                qmi_message_${service_lowercase}_abort_input_unref (input);\n'
                    '                qmi_message_unref (abort);\n'
                    '            }\n'
                    '\n')

            template += (
                '        g_task_return_error (task, error);\n'
                '        g_object_unref (task);\n'
                '        return;\n'
                '    }\n'
                '\n'
                '    /* Parse reply */\n'
                '    output = __${message_fullname_underscore}_response_parse (reply, &error);\n'
                '    if (!output)\n'
                '        g_task_return_error (task, error);\n'
                '    else\n'
                '        g_task_return_pointer (task,\n'
                '                               output,\n'
                '                               (GDestroyNotify)${output_underscore}_unref);\n'
                '    g_object_unref (task);\n'
                '    qmi_message_unref (reply);\n'
                '}\n'
                '\n'
                'void\n'
                '${underscore}_${message_underscore} (\n'
                '    ${camelcase} *self,\n'
                '    ${input_arg},\n'
                '    guint timeout,\n'
                '    GCancellable *cancellable,\n'
                '    GAsyncReadyCallback callback,\n'
                '    gpointer user_data)\n'
                '{\n'
                '    GTask *task;\n'
                '    QmiMessage *request;\n'
                '    GError *error = NULL;\n'
                '    guint16 transaction_id;\n')

            if message.vendor is not None:
                template += (
                    '    QmiMessageContext *context;\n')

            template += (
                '\n'
                '    task = g_task_new (self, cancellable, callback, user_data);\n'
                '\n'
                '    transaction_id = qmi_client_get_next_transaction_id (QMI_CLIENT (self));\n'
                '\n'
                '    request = __${message_fullname_underscore}_request_create (\n'
                '                  transaction_id,\n'
                '                  qmi_client_get_cid (QMI_CLIENT (self)),\n'
                '                  ${input_var},\n'
                '                  &error);\n'
                '    if (!request) {\n'
                '        g_prefix_error (&error, "Couldn\'t create request message: ");\n'
                '        g_task_return_error (task, error);\n'
                '        g_object_unref (task);\n'
                '        return;\n'
                '    }\n')

            if message.abort:
                template += (
                    '\n'
                    '    g_task_set_task_data (task, GUINT_TO_POINTER (transaction_id), NULL);\n')

            if message.vendor is not None:
                template += (
                    '\n'
                    '    context = qmi_message_context_new ();\n'
                    '    qmi_message_context_set_vendor_id (context, ${message_vendor_id});\n')

            template += (
                '\n'
                '    qmi_device_command_full (QMI_DEVICE (qmi_client_peek_device (QMI_CLIENT (self))),\n'
                '                             request,\n')

            if message.vendor is not None:
                template += (
                    '                             context,\n')
            else:
                template += (
                    '                             NULL,\n')

            template += (
                '                             timeout,\n'
                '                             cancellable,\n'
                '                             (GAsyncReadyCallback)${message_underscore}_ready,\n'
                '                             task);\n'
                '    qmi_message_unref (request);\n')

            if message.vendor is not None:
                template += (
                    '    qmi_message_context_unref (context);\n')

            template += (
                '}\n'
                '\n')
            cfile.write(string.Template(template).substitute(translations))


    """
    Emit the service-specific client implementation
    """
    def emit(self, hfile, cfile, message_list):
        # First, emit common class code
        utils.add_separator(hfile, 'CLIENT', self.name);
        utils.add_separator(cfile, 'CLIENT', self.name);
        self.__emit_class(hfile, cfile, message_list)
        self.__emit_methods(hfile, cfile, message_list)


    """
    Emit the sections
    """
    def emit_sections(self, sfile):
        translations = { 'underscore'                 : utils.build_underscore_name(self.name),
                         'no_prefix_underscore_upper' : utils.build_underscore_name(self.name[4:]).upper(),
                         'camelcase'                  : utils.build_camelcase_name (self.name),
                         'hyphened'                   : utils.build_dashed_name (self.name) }

        template = (
            '<SECTION>\n'
            '<FILE>${hyphened}</FILE>\n'
            '<TITLE>${camelcase}</TITLE>\n'
            '${camelcase}\n'
            '<SUBSECTION Standard>\n'
            '${camelcase}Class\n'
            'QMI_TYPE_${no_prefix_underscore_upper}\n'
            'QMI_${no_prefix_underscore_upper}\n'
            'QMI_${no_prefix_underscore_upper}_CLASS\n'
            'QMI_IS_${no_prefix_underscore_upper}\n'
            'QMI_IS_${no_prefix_underscore_upper}_CLASS\n'
            'QMI_${no_prefix_underscore_upper}_GET_CLASS\n'
            '${underscore}_get_type\n'
            '</SECTION>\n'
            '\n')
        sfile.write(string.Template(template).substitute(translations))
