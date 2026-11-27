// SPDX-License-Identifier: LGPL-2.1-or-later
/* ntos: the request handlers, in the protocol's order (made by tools/ntos/genhandlers.py) */
#include "ntos.h"

DECL_HANDLER(new_process);
DECL_HANDLER(get_new_process_info);
DECL_HANDLER(new_thread);
DECL_HANDLER(get_startup_info);
DECL_HANDLER(init_process_done);
DECL_HANDLER(init_first_thread);
DECL_HANDLER(init_thread);
DECL_HANDLER(terminate_process);
DECL_HANDLER(terminate_thread);
DECL_HANDLER(get_process_info);
DECL_HANDLER(get_process_debug_info);
DECL_HANDLER(get_process_image_name);
DECL_HANDLER(get_process_vm_counters);
DECL_HANDLER(set_process_info);
DECL_HANDLER(get_thread_info);
DECL_HANDLER(get_thread_times);
DECL_HANDLER(set_thread_info);
DECL_HANDLER(suspend_thread);
DECL_HANDLER(resume_thread);
DECL_HANDLER(queue_apc);
DECL_HANDLER(get_apc_result);
DECL_HANDLER(close_handle);
DECL_HANDLER(set_handle_info);
DECL_HANDLER(dup_handle);
DECL_HANDLER(allocate_reserve_object);
DECL_HANDLER(compare_objects);
DECL_HANDLER(set_object_permanence);
DECL_HANDLER(open_process);
DECL_HANDLER(open_thread);
DECL_HANDLER(select);
DECL_HANDLER(create_event);
DECL_HANDLER(event_op);
DECL_HANDLER(query_event);
DECL_HANDLER(open_event);
DECL_HANDLER(create_keyed_event);
DECL_HANDLER(open_keyed_event);
DECL_HANDLER(create_mutex);
DECL_HANDLER(release_mutex);
DECL_HANDLER(open_mutex);
DECL_HANDLER(query_mutex);
DECL_HANDLER(create_semaphore);
DECL_HANDLER(release_semaphore);
DECL_HANDLER(query_semaphore);
DECL_HANDLER(open_semaphore);
DECL_HANDLER(create_file);
DECL_HANDLER(open_file_object);
DECL_HANDLER(alloc_file_handle);
DECL_HANDLER(get_handle_unix_name);
DECL_HANDLER(get_handle_fd);
DECL_HANDLER(get_directory_cache_entry);
DECL_HANDLER(flush);
DECL_HANDLER(get_file_info);
DECL_HANDLER(get_volume_info);
DECL_HANDLER(lock_file);
DECL_HANDLER(unlock_file);
DECL_HANDLER(recv_socket);
DECL_HANDLER(send_socket);
DECL_HANDLER(socket_get_events);
DECL_HANDLER(socket_send_icmp_id);
DECL_HANDLER(socket_get_icmp_id);
DECL_HANDLER(get_next_console_request);
DECL_HANDLER(read_directory_changes);
DECL_HANDLER(read_change);
DECL_HANDLER(create_mapping);
DECL_HANDLER(open_mapping);
DECL_HANDLER(get_mapping_info);
DECL_HANDLER(get_image_map_address);
DECL_HANDLER(map_view);
DECL_HANDLER(map_image_view);
DECL_HANDLER(map_builtin_view);
DECL_HANDLER(get_image_view_info);
DECL_HANDLER(unmap_view);
DECL_HANDLER(get_mapping_committed_range);
DECL_HANDLER(add_mapping_committed_range);
DECL_HANDLER(is_same_mapping);
DECL_HANDLER(get_mapping_filename);
DECL_HANDLER(list_processes);
DECL_HANDLER(create_debug_obj);
DECL_HANDLER(wait_debug_event);
DECL_HANDLER(queue_exception_event);
DECL_HANDLER(get_exception_status);
DECL_HANDLER(continue_debug_event);
DECL_HANDLER(debug_process);
DECL_HANDLER(set_debug_obj_info);
DECL_HANDLER(read_process_memory);
DECL_HANDLER(write_process_memory);
DECL_HANDLER(create_key);
DECL_HANDLER(open_key);
DECL_HANDLER(delete_key);
DECL_HANDLER(flush_key);
DECL_HANDLER(enum_key);
DECL_HANDLER(set_key_value);
DECL_HANDLER(get_key_value);
DECL_HANDLER(get_key_values);
DECL_HANDLER(enum_key_value);
DECL_HANDLER(delete_key_value);
DECL_HANDLER(load_registry);
DECL_HANDLER(unload_registry);
DECL_HANDLER(save_registry);
DECL_HANDLER(set_registry_notification);
DECL_HANDLER(rename_key);
DECL_HANDLER(create_timer);
DECL_HANDLER(open_timer);
DECL_HANDLER(set_timer);
DECL_HANDLER(cancel_timer);
DECL_HANDLER(get_timer_info);
DECL_HANDLER(get_thread_context);
DECL_HANDLER(set_thread_context);
DECL_HANDLER(get_process_idle_event);
DECL_HANDLER(get_serial_info);
DECL_HANDLER(set_serial_info);
DECL_HANDLER(cancel_sync);
DECL_HANDLER(register_async);
DECL_HANDLER(cancel_async);
DECL_HANDLER(get_async_result);
DECL_HANDLER(set_async_direct_result);
DECL_HANDLER(read);
DECL_HANDLER(write);
DECL_HANDLER(ioctl);
DECL_HANDLER(set_irp_result);
DECL_HANDLER(create_named_pipe);
DECL_HANDLER(set_named_pipe_info);
DECL_HANDLER(create_winstation);
DECL_HANDLER(open_winstation);
DECL_HANDLER(close_winstation);
DECL_HANDLER(set_winstation_monitors);
DECL_HANDLER(get_process_winstation);
DECL_HANDLER(set_process_winstation);
DECL_HANDLER(enum_winstation);
DECL_HANDLER(create_desktop);
DECL_HANDLER(open_desktop);
DECL_HANDLER(open_input_desktop);
DECL_HANDLER(set_input_desktop);
DECL_HANDLER(close_desktop);
DECL_HANDLER(get_thread_desktop);
DECL_HANDLER(set_thread_desktop);
DECL_HANDLER(set_user_object_info);
DECL_HANDLER(create_token);
DECL_HANDLER(open_token);
DECL_HANDLER(adjust_token_privileges);
DECL_HANDLER(get_token_privileges);
DECL_HANDLER(check_token_privileges);
DECL_HANDLER(duplicate_token);
DECL_HANDLER(filter_token);
DECL_HANDLER(access_check);
DECL_HANDLER(get_token_sid);
DECL_HANDLER(get_token_groups);
DECL_HANDLER(get_token_default_dacl);
DECL_HANDLER(set_token_default_dacl);
DECL_HANDLER(set_security_object);
DECL_HANDLER(get_security_object);
DECL_HANDLER(get_system_handles);
DECL_HANDLER(get_tcp_connections);
DECL_HANDLER(get_udp_endpoints);
DECL_HANDLER(create_mailslot);
DECL_HANDLER(set_mailslot_info);
DECL_HANDLER(create_directory);
DECL_HANDLER(open_directory);
DECL_HANDLER(get_directory_entries);
DECL_HANDLER(create_symlink);
DECL_HANDLER(open_symlink);
DECL_HANDLER(query_symlink);
DECL_HANDLER(get_object_info);
DECL_HANDLER(get_object_name);
DECL_HANDLER(get_object_type);
DECL_HANDLER(get_object_types);
DECL_HANDLER(allocate_locally_unique_id);
DECL_HANDLER(create_device_manager);
DECL_HANDLER(create_device);
DECL_HANDLER(set_device_unix_path);
DECL_HANDLER(delete_device);
DECL_HANDLER(get_next_device_request);
DECL_HANDLER(get_kernel_object_ptr);
DECL_HANDLER(set_kernel_object_ptr);
DECL_HANDLER(grab_kernel_object);
DECL_HANDLER(release_kernel_object);
DECL_HANDLER(get_kernel_object_handle);
DECL_HANDLER(make_process_system);
DECL_HANDLER(grant_process_admin_token);
DECL_HANDLER(get_token_info);
DECL_HANDLER(create_linked_token);
DECL_HANDLER(create_completion);
DECL_HANDLER(open_completion);
DECL_HANDLER(add_completion);
DECL_HANDLER(remove_completion);
DECL_HANDLER(get_thread_completion);
DECL_HANDLER(query_completion);
DECL_HANDLER(set_completion_info);
DECL_HANDLER(add_fd_completion);
DECL_HANDLER(set_fd_completion_mode);
DECL_HANDLER(set_fd_disp_info);
DECL_HANDLER(set_fd_name_info);
DECL_HANDLER(set_fd_eof_info);
DECL_HANDLER(create_job);
DECL_HANDLER(open_job);
DECL_HANDLER(assign_job);
DECL_HANDLER(process_in_job);
DECL_HANDLER(set_job_limits);
DECL_HANDLER(set_job_completion_port);
DECL_HANDLER(get_job_info);
DECL_HANDLER(terminate_job);
DECL_HANDLER(suspend_process);
DECL_HANDLER(resume_process);
DECL_HANDLER(get_next_process);
DECL_HANDLER(get_next_thread);
DECL_HANDLER(get_inproc_sync_fd);
DECL_HANDLER(get_inproc_alert_fd);
DECL_HANDLER(d3dkmt_object_create);
DECL_HANDLER(d3dkmt_object_update);
DECL_HANDLER(d3dkmt_object_query);
DECL_HANDLER(d3dkmt_object_open);
DECL_HANDLER(d3dkmt_share_objects);
DECL_HANDLER(d3dkmt_object_open_name);
DECL_HANDLER(d3dkmt_mutex_acquire);
DECL_HANDLER(d3dkmt_mutex_release);
DECL_HANDLER(alpc_create_port);
DECL_HANDLER(register_fastcall);
DECL_HANDLER(get_desktop_id);

static void w_new_process( const void *req, void *reply ) { req_new_process( req, reply ); }
static void w_get_new_process_info( const void *req, void *reply ) { req_get_new_process_info( req, reply ); }
static void w_new_thread( const void *req, void *reply ) { req_new_thread( req, reply ); }
static void w_get_startup_info( const void *req, void *reply ) { req_get_startup_info( req, reply ); }
static void w_init_process_done( const void *req, void *reply ) { req_init_process_done( req, reply ); }
static void w_init_first_thread( const void *req, void *reply ) { req_init_first_thread( req, reply ); }
static void w_init_thread( const void *req, void *reply ) { req_init_thread( req, reply ); }
static void w_terminate_process( const void *req, void *reply ) { req_terminate_process( req, reply ); }
static void w_terminate_thread( const void *req, void *reply ) { req_terminate_thread( req, reply ); }
static void w_get_process_info( const void *req, void *reply ) { req_get_process_info( req, reply ); }
static void w_get_process_debug_info( const void *req, void *reply ) { req_get_process_debug_info( req, reply ); }
static void w_get_process_image_name( const void *req, void *reply ) { req_get_process_image_name( req, reply ); }
static void w_get_process_vm_counters( const void *req, void *reply ) { req_get_process_vm_counters( req, reply ); }
static void w_set_process_info( const void *req, void *reply ) { req_set_process_info( req, reply ); }
static void w_get_thread_info( const void *req, void *reply ) { req_get_thread_info( req, reply ); }
static void w_get_thread_times( const void *req, void *reply ) { req_get_thread_times( req, reply ); }
static void w_set_thread_info( const void *req, void *reply ) { req_set_thread_info( req, reply ); }
static void w_suspend_thread( const void *req, void *reply ) { req_suspend_thread( req, reply ); }
static void w_resume_thread( const void *req, void *reply ) { req_resume_thread( req, reply ); }
static void w_queue_apc( const void *req, void *reply ) { req_queue_apc( req, reply ); }
static void w_get_apc_result( const void *req, void *reply ) { req_get_apc_result( req, reply ); }
static void w_close_handle( const void *req, void *reply ) { req_close_handle( req, reply ); }
static void w_set_handle_info( const void *req, void *reply ) { req_set_handle_info( req, reply ); }
static void w_dup_handle( const void *req, void *reply ) { req_dup_handle( req, reply ); }
static void w_allocate_reserve_object( const void *req, void *reply ) { req_allocate_reserve_object( req, reply ); }
static void w_compare_objects( const void *req, void *reply ) { req_compare_objects( req, reply ); }
static void w_set_object_permanence( const void *req, void *reply ) { req_set_object_permanence( req, reply ); }
static void w_open_process( const void *req, void *reply ) { req_open_process( req, reply ); }
static void w_open_thread( const void *req, void *reply ) { req_open_thread( req, reply ); }
static void w_select( const void *req, void *reply ) { req_select( req, reply ); }
static void w_create_event( const void *req, void *reply ) { req_create_event( req, reply ); }
static void w_event_op( const void *req, void *reply ) { req_event_op( req, reply ); }
static void w_query_event( const void *req, void *reply ) { req_query_event( req, reply ); }
static void w_open_event( const void *req, void *reply ) { req_open_event( req, reply ); }
static void w_create_keyed_event( const void *req, void *reply ) { req_create_keyed_event( req, reply ); }
static void w_open_keyed_event( const void *req, void *reply ) { req_open_keyed_event( req, reply ); }
static void w_create_mutex( const void *req, void *reply ) { req_create_mutex( req, reply ); }
static void w_release_mutex( const void *req, void *reply ) { req_release_mutex( req, reply ); }
static void w_open_mutex( const void *req, void *reply ) { req_open_mutex( req, reply ); }
static void w_query_mutex( const void *req, void *reply ) { req_query_mutex( req, reply ); }
static void w_create_semaphore( const void *req, void *reply ) { req_create_semaphore( req, reply ); }
static void w_release_semaphore( const void *req, void *reply ) { req_release_semaphore( req, reply ); }
static void w_query_semaphore( const void *req, void *reply ) { req_query_semaphore( req, reply ); }
static void w_open_semaphore( const void *req, void *reply ) { req_open_semaphore( req, reply ); }
static void w_create_file( const void *req, void *reply ) { req_create_file( req, reply ); }
static void w_open_file_object( const void *req, void *reply ) { req_open_file_object( req, reply ); }
static void w_alloc_file_handle( const void *req, void *reply ) { req_alloc_file_handle( req, reply ); }
static void w_get_handle_unix_name( const void *req, void *reply ) { req_get_handle_unix_name( req, reply ); }
static void w_get_handle_fd( const void *req, void *reply ) { req_get_handle_fd( req, reply ); }
static void w_get_directory_cache_entry( const void *req, void *reply ) { req_get_directory_cache_entry( req, reply ); }
static void w_flush( const void *req, void *reply ) { req_flush( req, reply ); }
static void w_get_file_info( const void *req, void *reply ) { req_get_file_info( req, reply ); }
static void w_get_volume_info( const void *req, void *reply ) { req_get_volume_info( req, reply ); }
static void w_lock_file( const void *req, void *reply ) { req_lock_file( req, reply ); }
static void w_unlock_file( const void *req, void *reply ) { req_unlock_file( req, reply ); }
static void w_recv_socket( const void *req, void *reply ) { req_recv_socket( req, reply ); }
static void w_send_socket( const void *req, void *reply ) { req_send_socket( req, reply ); }
static void w_socket_get_events( const void *req, void *reply ) { req_socket_get_events( req, reply ); }
static void w_socket_send_icmp_id( const void *req, void *reply ) { req_socket_send_icmp_id( req, reply ); }
static void w_socket_get_icmp_id( const void *req, void *reply ) { req_socket_get_icmp_id( req, reply ); }
static void w_get_next_console_request( const void *req, void *reply ) { req_get_next_console_request( req, reply ); }
static void w_read_directory_changes( const void *req, void *reply ) { req_read_directory_changes( req, reply ); }
static void w_read_change( const void *req, void *reply ) { req_read_change( req, reply ); }
static void w_create_mapping( const void *req, void *reply ) { req_create_mapping( req, reply ); }
static void w_open_mapping( const void *req, void *reply ) { req_open_mapping( req, reply ); }
static void w_get_mapping_info( const void *req, void *reply ) { req_get_mapping_info( req, reply ); }
static void w_get_image_map_address( const void *req, void *reply ) { req_get_image_map_address( req, reply ); }
static void w_map_view( const void *req, void *reply ) { req_map_view( req, reply ); }
static void w_map_image_view( const void *req, void *reply ) { req_map_image_view( req, reply ); }
static void w_map_builtin_view( const void *req, void *reply ) { req_map_builtin_view( req, reply ); }
static void w_get_image_view_info( const void *req, void *reply ) { req_get_image_view_info( req, reply ); }
static void w_unmap_view( const void *req, void *reply ) { req_unmap_view( req, reply ); }
static void w_get_mapping_committed_range( const void *req, void *reply ) { req_get_mapping_committed_range( req, reply ); }
static void w_add_mapping_committed_range( const void *req, void *reply ) { req_add_mapping_committed_range( req, reply ); }
static void w_is_same_mapping( const void *req, void *reply ) { req_is_same_mapping( req, reply ); }
static void w_get_mapping_filename( const void *req, void *reply ) { req_get_mapping_filename( req, reply ); }
static void w_list_processes( const void *req, void *reply ) { req_list_processes( req, reply ); }
static void w_create_debug_obj( const void *req, void *reply ) { req_create_debug_obj( req, reply ); }
static void w_wait_debug_event( const void *req, void *reply ) { req_wait_debug_event( req, reply ); }
static void w_queue_exception_event( const void *req, void *reply ) { req_queue_exception_event( req, reply ); }
static void w_get_exception_status( const void *req, void *reply ) { req_get_exception_status( req, reply ); }
static void w_continue_debug_event( const void *req, void *reply ) { req_continue_debug_event( req, reply ); }
static void w_debug_process( const void *req, void *reply ) { req_debug_process( req, reply ); }
static void w_set_debug_obj_info( const void *req, void *reply ) { req_set_debug_obj_info( req, reply ); }
static void w_read_process_memory( const void *req, void *reply ) { req_read_process_memory( req, reply ); }
static void w_write_process_memory( const void *req, void *reply ) { req_write_process_memory( req, reply ); }
static void w_create_key( const void *req, void *reply ) { req_create_key( req, reply ); }
static void w_open_key( const void *req, void *reply ) { req_open_key( req, reply ); }
static void w_delete_key( const void *req, void *reply ) { req_delete_key( req, reply ); }
static void w_flush_key( const void *req, void *reply ) { req_flush_key( req, reply ); }
static void w_enum_key( const void *req, void *reply ) { req_enum_key( req, reply ); }
static void w_set_key_value( const void *req, void *reply ) { req_set_key_value( req, reply ); }
static void w_get_key_value( const void *req, void *reply ) { req_get_key_value( req, reply ); }
static void w_get_key_values( const void *req, void *reply ) { req_get_key_values( req, reply ); }
static void w_enum_key_value( const void *req, void *reply ) { req_enum_key_value( req, reply ); }
static void w_delete_key_value( const void *req, void *reply ) { req_delete_key_value( req, reply ); }
static void w_load_registry( const void *req, void *reply ) { req_load_registry( req, reply ); }
static void w_unload_registry( const void *req, void *reply ) { req_unload_registry( req, reply ); }
static void w_save_registry( const void *req, void *reply ) { req_save_registry( req, reply ); }
static void w_set_registry_notification( const void *req, void *reply ) { req_set_registry_notification( req, reply ); }
static void w_rename_key( const void *req, void *reply ) { req_rename_key( req, reply ); }
static void w_create_timer( const void *req, void *reply ) { req_create_timer( req, reply ); }
static void w_open_timer( const void *req, void *reply ) { req_open_timer( req, reply ); }
static void w_set_timer( const void *req, void *reply ) { req_set_timer( req, reply ); }
static void w_cancel_timer( const void *req, void *reply ) { req_cancel_timer( req, reply ); }
static void w_get_timer_info( const void *req, void *reply ) { req_get_timer_info( req, reply ); }
static void w_get_thread_context( const void *req, void *reply ) { req_get_thread_context( req, reply ); }
static void w_set_thread_context( const void *req, void *reply ) { req_set_thread_context( req, reply ); }
static void w_get_process_idle_event( const void *req, void *reply ) { req_get_process_idle_event( req, reply ); }
static void w_get_serial_info( const void *req, void *reply ) { req_get_serial_info( req, reply ); }
static void w_set_serial_info( const void *req, void *reply ) { req_set_serial_info( req, reply ); }
static void w_cancel_sync( const void *req, void *reply ) { req_cancel_sync( req, reply ); }
static void w_register_async( const void *req, void *reply ) { req_register_async( req, reply ); }
static void w_cancel_async( const void *req, void *reply ) { req_cancel_async( req, reply ); }
static void w_get_async_result( const void *req, void *reply ) { req_get_async_result( req, reply ); }
static void w_set_async_direct_result( const void *req, void *reply ) { req_set_async_direct_result( req, reply ); }
static void w_read( const void *req, void *reply ) { req_read( req, reply ); }
static void w_write( const void *req, void *reply ) { req_write( req, reply ); }
static void w_ioctl( const void *req, void *reply ) { req_ioctl( req, reply ); }
static void w_set_irp_result( const void *req, void *reply ) { req_set_irp_result( req, reply ); }
static void w_create_named_pipe( const void *req, void *reply ) { req_create_named_pipe( req, reply ); }
static void w_set_named_pipe_info( const void *req, void *reply ) { req_set_named_pipe_info( req, reply ); }
static void w_create_winstation( const void *req, void *reply ) { req_create_winstation( req, reply ); }
static void w_open_winstation( const void *req, void *reply ) { req_open_winstation( req, reply ); }
static void w_close_winstation( const void *req, void *reply ) { req_close_winstation( req, reply ); }
static void w_set_winstation_monitors( const void *req, void *reply ) { req_set_winstation_monitors( req, reply ); }
static void w_get_process_winstation( const void *req, void *reply ) { req_get_process_winstation( req, reply ); }
static void w_set_process_winstation( const void *req, void *reply ) { req_set_process_winstation( req, reply ); }
static void w_enum_winstation( const void *req, void *reply ) { req_enum_winstation( req, reply ); }
static void w_create_desktop( const void *req, void *reply ) { req_create_desktop( req, reply ); }
static void w_open_desktop( const void *req, void *reply ) { req_open_desktop( req, reply ); }
static void w_open_input_desktop( const void *req, void *reply ) { req_open_input_desktop( req, reply ); }
static void w_set_input_desktop( const void *req, void *reply ) { req_set_input_desktop( req, reply ); }
static void w_close_desktop( const void *req, void *reply ) { req_close_desktop( req, reply ); }
static void w_get_thread_desktop( const void *req, void *reply ) { req_get_thread_desktop( req, reply ); }
static void w_set_thread_desktop( const void *req, void *reply ) { req_set_thread_desktop( req, reply ); }
static void w_set_user_object_info( const void *req, void *reply ) { req_set_user_object_info( req, reply ); }
static void w_create_token( const void *req, void *reply ) { req_create_token( req, reply ); }
static void w_open_token( const void *req, void *reply ) { req_open_token( req, reply ); }
static void w_adjust_token_privileges( const void *req, void *reply ) { req_adjust_token_privileges( req, reply ); }
static void w_get_token_privileges( const void *req, void *reply ) { req_get_token_privileges( req, reply ); }
static void w_check_token_privileges( const void *req, void *reply ) { req_check_token_privileges( req, reply ); }
static void w_duplicate_token( const void *req, void *reply ) { req_duplicate_token( req, reply ); }
static void w_filter_token( const void *req, void *reply ) { req_filter_token( req, reply ); }
static void w_access_check( const void *req, void *reply ) { req_access_check( req, reply ); }
static void w_get_token_sid( const void *req, void *reply ) { req_get_token_sid( req, reply ); }
static void w_get_token_groups( const void *req, void *reply ) { req_get_token_groups( req, reply ); }
static void w_get_token_default_dacl( const void *req, void *reply ) { req_get_token_default_dacl( req, reply ); }
static void w_set_token_default_dacl( const void *req, void *reply ) { req_set_token_default_dacl( req, reply ); }
static void w_set_security_object( const void *req, void *reply ) { req_set_security_object( req, reply ); }
static void w_get_security_object( const void *req, void *reply ) { req_get_security_object( req, reply ); }
static void w_get_system_handles( const void *req, void *reply ) { req_get_system_handles( req, reply ); }
static void w_get_tcp_connections( const void *req, void *reply ) { req_get_tcp_connections( req, reply ); }
static void w_get_udp_endpoints( const void *req, void *reply ) { req_get_udp_endpoints( req, reply ); }
static void w_create_mailslot( const void *req, void *reply ) { req_create_mailslot( req, reply ); }
static void w_set_mailslot_info( const void *req, void *reply ) { req_set_mailslot_info( req, reply ); }
static void w_create_directory( const void *req, void *reply ) { req_create_directory( req, reply ); }
static void w_open_directory( const void *req, void *reply ) { req_open_directory( req, reply ); }
static void w_get_directory_entries( const void *req, void *reply ) { req_get_directory_entries( req, reply ); }
static void w_create_symlink( const void *req, void *reply ) { req_create_symlink( req, reply ); }
static void w_open_symlink( const void *req, void *reply ) { req_open_symlink( req, reply ); }
static void w_query_symlink( const void *req, void *reply ) { req_query_symlink( req, reply ); }
static void w_get_object_info( const void *req, void *reply ) { req_get_object_info( req, reply ); }
static void w_get_object_name( const void *req, void *reply ) { req_get_object_name( req, reply ); }
static void w_get_object_type( const void *req, void *reply ) { req_get_object_type( req, reply ); }
static void w_get_object_types( const void *req, void *reply ) { req_get_object_types( req, reply ); }
static void w_allocate_locally_unique_id( const void *req, void *reply ) { req_allocate_locally_unique_id( req, reply ); }
static void w_create_device_manager( const void *req, void *reply ) { req_create_device_manager( req, reply ); }
static void w_create_device( const void *req, void *reply ) { req_create_device( req, reply ); }
static void w_set_device_unix_path( const void *req, void *reply ) { req_set_device_unix_path( req, reply ); }
static void w_delete_device( const void *req, void *reply ) { req_delete_device( req, reply ); }
static void w_get_next_device_request( const void *req, void *reply ) { req_get_next_device_request( req, reply ); }
static void w_get_kernel_object_ptr( const void *req, void *reply ) { req_get_kernel_object_ptr( req, reply ); }
static void w_set_kernel_object_ptr( const void *req, void *reply ) { req_set_kernel_object_ptr( req, reply ); }
static void w_grab_kernel_object( const void *req, void *reply ) { req_grab_kernel_object( req, reply ); }
static void w_release_kernel_object( const void *req, void *reply ) { req_release_kernel_object( req, reply ); }
static void w_get_kernel_object_handle( const void *req, void *reply ) { req_get_kernel_object_handle( req, reply ); }
static void w_make_process_system( const void *req, void *reply ) { req_make_process_system( req, reply ); }
static void w_grant_process_admin_token( const void *req, void *reply ) { req_grant_process_admin_token( req, reply ); }
static void w_get_token_info( const void *req, void *reply ) { req_get_token_info( req, reply ); }
static void w_create_linked_token( const void *req, void *reply ) { req_create_linked_token( req, reply ); }
static void w_create_completion( const void *req, void *reply ) { req_create_completion( req, reply ); }
static void w_open_completion( const void *req, void *reply ) { req_open_completion( req, reply ); }
static void w_add_completion( const void *req, void *reply ) { req_add_completion( req, reply ); }
static void w_remove_completion( const void *req, void *reply ) { req_remove_completion( req, reply ); }
static void w_get_thread_completion( const void *req, void *reply ) { req_get_thread_completion( req, reply ); }
static void w_query_completion( const void *req, void *reply ) { req_query_completion( req, reply ); }
static void w_set_completion_info( const void *req, void *reply ) { req_set_completion_info( req, reply ); }
static void w_add_fd_completion( const void *req, void *reply ) { req_add_fd_completion( req, reply ); }
static void w_set_fd_completion_mode( const void *req, void *reply ) { req_set_fd_completion_mode( req, reply ); }
static void w_set_fd_disp_info( const void *req, void *reply ) { req_set_fd_disp_info( req, reply ); }
static void w_set_fd_name_info( const void *req, void *reply ) { req_set_fd_name_info( req, reply ); }
static void w_set_fd_eof_info( const void *req, void *reply ) { req_set_fd_eof_info( req, reply ); }
static void w_create_job( const void *req, void *reply ) { req_create_job( req, reply ); }
static void w_open_job( const void *req, void *reply ) { req_open_job( req, reply ); }
static void w_assign_job( const void *req, void *reply ) { req_assign_job( req, reply ); }
static void w_process_in_job( const void *req, void *reply ) { req_process_in_job( req, reply ); }
static void w_set_job_limits( const void *req, void *reply ) { req_set_job_limits( req, reply ); }
static void w_set_job_completion_port( const void *req, void *reply ) { req_set_job_completion_port( req, reply ); }
static void w_get_job_info( const void *req, void *reply ) { req_get_job_info( req, reply ); }
static void w_terminate_job( const void *req, void *reply ) { req_terminate_job( req, reply ); }
static void w_suspend_process( const void *req, void *reply ) { req_suspend_process( req, reply ); }
static void w_resume_process( const void *req, void *reply ) { req_resume_process( req, reply ); }
static void w_get_next_process( const void *req, void *reply ) { req_get_next_process( req, reply ); }
static void w_get_next_thread( const void *req, void *reply ) { req_get_next_thread( req, reply ); }
static void w_get_inproc_sync_fd( const void *req, void *reply ) { req_get_inproc_sync_fd( req, reply ); }
static void w_get_inproc_alert_fd( const void *req, void *reply ) { req_get_inproc_alert_fd( req, reply ); }
static void w_d3dkmt_object_create( const void *req, void *reply ) { req_d3dkmt_object_create( req, reply ); }
static void w_d3dkmt_object_update( const void *req, void *reply ) { req_d3dkmt_object_update( req, reply ); }
static void w_d3dkmt_object_query( const void *req, void *reply ) { req_d3dkmt_object_query( req, reply ); }
static void w_d3dkmt_object_open( const void *req, void *reply ) { req_d3dkmt_object_open( req, reply ); }
static void w_d3dkmt_share_objects( const void *req, void *reply ) { req_d3dkmt_share_objects( req, reply ); }
static void w_d3dkmt_object_open_name( const void *req, void *reply ) { req_d3dkmt_object_open_name( req, reply ); }
static void w_d3dkmt_mutex_acquire( const void *req, void *reply ) { req_d3dkmt_mutex_acquire( req, reply ); }
static void w_d3dkmt_mutex_release( const void *req, void *reply ) { req_d3dkmt_mutex_release( req, reply ); }
static void w_alpc_create_port( const void *req, void *reply ) { req_alpc_create_port( req, reply ); }
static void w_register_fastcall( const void *req, void *reply ) { req_register_fastcall( req, reply ); }
static void w_get_desktop_id( const void *req, void *reply ) { req_get_desktop_id( req, reply ); }

const ntos_handler ntos_handlers[REQ_NB_REQUESTS] =
{
    w_new_process,
    w_get_new_process_info,
    w_new_thread,
    w_get_startup_info,
    w_init_process_done,
    w_init_first_thread,
    w_init_thread,
    w_terminate_process,
    w_terminate_thread,
    w_get_process_info,
    w_get_process_debug_info,
    w_get_process_image_name,
    w_get_process_vm_counters,
    w_set_process_info,
    w_get_thread_info,
    w_get_thread_times,
    w_set_thread_info,
    w_suspend_thread,
    w_resume_thread,
    w_queue_apc,
    w_get_apc_result,
    w_close_handle,
    w_set_handle_info,
    w_dup_handle,
    w_allocate_reserve_object,
    w_compare_objects,
    w_set_object_permanence,
    w_open_process,
    w_open_thread,
    w_select,
    w_create_event,
    w_event_op,
    w_query_event,
    w_open_event,
    w_create_keyed_event,
    w_open_keyed_event,
    w_create_mutex,
    w_release_mutex,
    w_open_mutex,
    w_query_mutex,
    w_create_semaphore,
    w_release_semaphore,
    w_query_semaphore,
    w_open_semaphore,
    w_create_file,
    w_open_file_object,
    w_alloc_file_handle,
    w_get_handle_unix_name,
    w_get_handle_fd,
    w_get_directory_cache_entry,
    w_flush,
    w_get_file_info,
    w_get_volume_info,
    w_lock_file,
    w_unlock_file,
    w_recv_socket,
    w_send_socket,
    w_socket_get_events,
    w_socket_send_icmp_id,
    w_socket_get_icmp_id,
    w_get_next_console_request,
    w_read_directory_changes,
    w_read_change,
    w_create_mapping,
    w_open_mapping,
    w_get_mapping_info,
    w_get_image_map_address,
    w_map_view,
    w_map_image_view,
    w_map_builtin_view,
    w_get_image_view_info,
    w_unmap_view,
    w_get_mapping_committed_range,
    w_add_mapping_committed_range,
    w_is_same_mapping,
    w_get_mapping_filename,
    w_list_processes,
    w_create_debug_obj,
    w_wait_debug_event,
    w_queue_exception_event,
    w_get_exception_status,
    w_continue_debug_event,
    w_debug_process,
    w_set_debug_obj_info,
    w_read_process_memory,
    w_write_process_memory,
    w_create_key,
    w_open_key,
    w_delete_key,
    w_flush_key,
    w_enum_key,
    w_set_key_value,
    w_get_key_value,
    w_get_key_values,
    w_enum_key_value,
    w_delete_key_value,
    w_load_registry,
    w_unload_registry,
    w_save_registry,
    w_set_registry_notification,
    w_rename_key,
    w_create_timer,
    w_open_timer,
    w_set_timer,
    w_cancel_timer,
    w_get_timer_info,
    w_get_thread_context,
    w_set_thread_context,
    NULL,  /* add_atom: win32k's */
    NULL,  /* delete_atom: win32k's */
    NULL,  /* find_atom: win32k's */
    NULL,  /* get_atom_information: win32k's */
    NULL,  /* add_user_atom: win32k's */
    NULL,  /* get_user_atom_name: win32k's */
    NULL,  /* get_msg_queue_handle: win32k's */
    NULL,  /* get_msg_queue: win32k's */
    NULL,  /* set_queue_fd: win32k's */
    NULL,  /* set_queue_mask: win32k's */
    NULL,  /* get_queue_status: win32k's */
    w_get_process_idle_event,
    NULL,  /* send_message: win32k's */
    NULL,  /* post_quit_message: win32k's */
    NULL,  /* send_hardware_message: win32k's */
    NULL,  /* get_message: win32k's */
    NULL,  /* reply_message: win32k's */
    NULL,  /* accept_hardware_message: win32k's */
    NULL,  /* get_message_reply: win32k's */
    NULL,  /* set_win_timer: win32k's */
    NULL,  /* kill_win_timer: win32k's */
    NULL,  /* is_window_hung: win32k's */
    w_get_serial_info,
    w_set_serial_info,
    w_cancel_sync,
    w_register_async,
    w_cancel_async,
    w_get_async_result,
    w_set_async_direct_result,
    w_read,
    w_write,
    w_ioctl,
    w_set_irp_result,
    w_create_named_pipe,
    w_set_named_pipe_info,
    NULL,  /* create_window: win32k's */
    NULL,  /* destroy_window: win32k's */
    NULL,  /* get_desktop_window: win32k's */
    NULL,  /* set_window_owner: win32k's */
    NULL,  /* get_window_info: win32k's */
    NULL,  /* init_window_info: win32k's */
    NULL,  /* set_window_info: win32k's */
    NULL,  /* set_window_fnid: win32k's */
    NULL,  /* set_parent: win32k's */
    NULL,  /* get_window_parents: win32k's */
    NULL,  /* get_window_list: win32k's */
    NULL,  /* get_class_windows: win32k's */
    NULL,  /* get_window_children_from_point: win32k's */
    NULL,  /* register_shell_hook_window: win32k's */
    NULL,  /* set_window_composited: win32k's */
    NULL,  /* get_composition_list: win32k's */
    NULL,  /* set_snap_preview: win32k's */
    NULL,  /* set_window_band: win32k's */
    NULL,  /* get_window_tree: win32k's */
    NULL,  /* set_window_pos: win32k's */
    NULL,  /* get_window_rectangles: win32k's */
    NULL,  /* get_window_text: win32k's */
    NULL,  /* set_window_text: win32k's */
    NULL,  /* get_windows_offset: win32k's */
    NULL,  /* get_visible_region: win32k's */
    NULL,  /* get_window_region: win32k's */
    NULL,  /* set_window_region: win32k's */
    NULL,  /* get_update_region: win32k's */
    NULL,  /* update_window_zorder: win32k's */
    NULL,  /* redraw_window: win32k's */
    NULL,  /* set_window_property: win32k's */
    NULL,  /* remove_window_property: win32k's */
    NULL,  /* get_window_property: win32k's */
    NULL,  /* get_window_properties: win32k's */
    w_create_winstation,
    w_open_winstation,
    w_close_winstation,
    w_set_winstation_monitors,
    w_get_process_winstation,
    w_set_process_winstation,
    w_enum_winstation,
    w_create_desktop,
    w_open_desktop,
    w_open_input_desktop,
    w_set_input_desktop,
    w_close_desktop,
    w_get_thread_desktop,
    w_set_thread_desktop,
    w_set_user_object_info,
    NULL,  /* register_hotkey: win32k's */
    NULL,  /* unregister_hotkey: win32k's */
    NULL,  /* attach_thread_input: win32k's */
    NULL,  /* get_thread_input: win32k's */
    NULL,  /* set_user_input_time: win32k's */
    NULL,  /* get_key_state: win32k's */
    NULL,  /* set_key_state: win32k's */
    NULL,  /* set_foreground_window: win32k's */
    NULL,  /* allow_set_foreground: win32k's */
    NULL,  /* set_focus_window: win32k's */
    NULL,  /* set_active_window: win32k's */
    NULL,  /* set_capture_window: win32k's */
    NULL,  /* set_caret_window: win32k's */
    NULL,  /* set_caret_info: win32k's */
    NULL,  /* set_hook: win32k's */
    NULL,  /* remove_hook: win32k's */
    NULL,  /* start_hook_chain: win32k's */
    NULL,  /* finish_hook_chain: win32k's */
    NULL,  /* get_hook_info: win32k's */
    NULL,  /* create_class: win32k's */
    NULL,  /* destroy_class: win32k's */
    NULL,  /* set_class_info: win32k's */
    NULL,  /* get_class_info: win32k's */
    NULL,  /* open_clipboard: win32k's */
    NULL,  /* close_clipboard: win32k's */
    NULL,  /* empty_clipboard: win32k's */
    NULL,  /* set_clipboard_data: win32k's */
    NULL,  /* get_clipboard_data: win32k's */
    NULL,  /* get_clipboard_formats: win32k's */
    NULL,  /* enum_clipboard_formats: win32k's */
    NULL,  /* release_clipboard: win32k's */
    NULL,  /* get_clipboard_info: win32k's */
    NULL,  /* set_clipboard_viewer: win32k's */
    NULL,  /* add_clipboard_listener: win32k's */
    NULL,  /* remove_clipboard_listener: win32k's */
    w_create_token,
    w_open_token,
    NULL,  /* set_desktop_shell_windows: win32k's */
    w_adjust_token_privileges,
    w_get_token_privileges,
    w_check_token_privileges,
    w_duplicate_token,
    w_filter_token,
    w_access_check,
    w_get_token_sid,
    w_get_token_groups,
    w_get_token_default_dacl,
    w_set_token_default_dacl,
    w_set_security_object,
    w_get_security_object,
    w_get_system_handles,
    w_get_tcp_connections,
    w_get_udp_endpoints,
    w_create_mailslot,
    w_set_mailslot_info,
    w_create_directory,
    w_open_directory,
    w_get_directory_entries,
    w_create_symlink,
    w_open_symlink,
    w_query_symlink,
    w_get_object_info,
    w_get_object_name,
    w_get_object_type,
    w_get_object_types,
    w_allocate_locally_unique_id,
    w_create_device_manager,
    w_create_device,
    w_set_device_unix_path,
    w_delete_device,
    w_get_next_device_request,
    w_get_kernel_object_ptr,
    w_set_kernel_object_ptr,
    w_grab_kernel_object,
    w_release_kernel_object,
    w_get_kernel_object_handle,
    w_make_process_system,
    w_grant_process_admin_token,
    w_get_token_info,
    w_create_linked_token,
    w_create_completion,
    w_open_completion,
    w_add_completion,
    w_remove_completion,
    w_get_thread_completion,
    w_query_completion,
    w_set_completion_info,
    w_add_fd_completion,
    w_set_fd_completion_mode,
    w_set_fd_disp_info,
    w_set_fd_name_info,
    w_set_fd_eof_info,
    NULL,  /* get_window_layered_info: win32k's */
    NULL,  /* set_window_layered_info: win32k's */
    NULL,  /* alloc_user_handle: win32k's */
    NULL,  /* free_user_handle: win32k's */
    NULL,  /* set_cursor: win32k's */
    NULL,  /* get_cursor_history: win32k's */
    NULL,  /* get_rawinput_buffer: win32k's */
    NULL,  /* update_rawinput_devices: win32k's */
    w_create_job,
    w_open_job,
    w_assign_job,
    w_process_in_job,
    w_set_job_limits,
    w_set_job_completion_port,
    w_get_job_info,
    w_terminate_job,
    w_suspend_process,
    w_resume_process,
    w_get_next_process,
    w_get_next_thread,
    NULL,  /* set_keyboard_repeat: win32k's */
    w_get_inproc_sync_fd,
    w_get_inproc_alert_fd,
    w_d3dkmt_object_create,
    w_d3dkmt_object_update,
    w_d3dkmt_object_query,
    w_d3dkmt_object_open,
    w_d3dkmt_share_objects,
    w_d3dkmt_object_open_name,
    w_d3dkmt_mutex_acquire,
    w_d3dkmt_mutex_release,
    w_alpc_create_port,
    w_register_fastcall,
    w_get_desktop_id,
};
