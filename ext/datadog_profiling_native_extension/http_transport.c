#include <ruby.h>
#include <ruby/thread.h>
#include <datadog/profiling.h>
#include "helpers.h"
#include "libdatadog_helpers.h"
#include "ruby_helpers.h"
#include "encoded_profile.h"

// Used to report profiling data to Datadog.
// This file implements the native bits of the Datadog::Profiling::HttpTransport class

static VALUE ok_symbol = Qnil; // :ok in Ruby
static VALUE error_symbol = Qnil; // :error in Ruby

static VALUE library_version_string = Qnil;

typedef struct {
  ddog_prof_Exporter *exporter;
  ddog_prof_Exporter_Request_BuildResult *build_result;
  ddog_CancellationToken *cancel_token;
  ddog_prof_Exporter_SendResult result;
  bool send_ran;
} call_exporter_without_gvl_arguments;

static inline ddog_ByteSlice byte_slice_from_ruby_string(VALUE string);
static VALUE _native_validate_exporter(VALUE self, VALUE exporter_configuration);
static ddog_prof_Exporter_NewResult create_exporter(VALUE exporter_configuration, VALUE tags_as_array);
static VALUE handle_exporter_failure(ddog_prof_Exporter_NewResult exporter_result);
static VALUE _native_do_export(
  VALUE self,
  VALUE exporter_configuration,
  VALUE upload_timeout_milliseconds,
  VALUE flush,
  VALUE start_timespec_seconds,
  VALUE start_timespec_nanoseconds,
  VALUE finish_timespec_seconds,
  VALUE finish_timespec_nanoseconds
);
static void *call_exporter_without_gvl(void *call_args);
static void interrupt_exporter_call(void *cancel_token);

void http_transport_init(VALUE profiling_module) {
  VALUE http_transport_class = rb_define_class_under(profiling_module, "HttpTransport", rb_cObject);

  rb_define_singleton_method(http_transport_class, "_native_validate_exporter",  _native_validate_exporter, 1);
  rb_define_singleton_method(http_transport_class, "_native_do_export",  _native_do_export, 7);

  ok_symbol = ID2SYM(rb_intern_const("ok"));
  error_symbol = ID2SYM(rb_intern_const("error"));

  library_version_string = datadog_gem_version();
  rb_global_variable(&library_version_string);
}

static inline ddog_ByteSlice byte_slice_from_ruby_string(VALUE string) {
  ENFORCE_TYPE(string, T_STRING);
  ddog_ByteSlice byte_slice = {.ptr = (uint8_t *) StringValuePtr(string), .len = RSTRING_LEN(string)};
  return byte_slice;
}

static VALUE _native_validate_exporter(DDTRACE_UNUSED VALUE _self, VALUE exporter_configuration) {
  ENFORCE_TYPE(exporter_configuration, T_ARRAY);
  ddog_prof_Exporter_NewResult exporter_result = create_exporter(exporter_configuration, rb_ary_new());

  VALUE failure_tuple = handle_exporter_failure(exporter_result);
  if (!NIL_P(failure_tuple)) return failure_tuple;

  // We don't actually need the exporter for now -- we just wanted to validate that we could create it with the
  // settings we were given
  ddog_prof_Exporter_drop(exporter_result.ok);

  return rb_ary_new_from_args(2, ok_symbol, Qnil);
}

static ddog_prof_Endpoint endpoint_from(VALUE exporter_configuration) {
  ENFORCE_TYPE(exporter_configuration, T_ARRAY);

  VALUE exporter_working_mode = rb_ary_entry(exporter_configuration, 0);
  ENFORCE_TYPE(exporter_working_mode, T_SYMBOL);
  ID working_mode = SYM2ID(exporter_working_mode);

  if (working_mode == rb_intern("agentless")) {
    VALUE site = rb_ary_entry(exporter_configuration, 1);
    VALUE api_key = rb_ary_entry(exporter_configuration, 2);

    return ddog_prof_Endpoint_agentless(char_slice_from_ruby_string(site), char_slice_from_ruby_string(api_key));
  } else if (working_mode == rb_intern("agent")) {
    VALUE base_url = rb_ary_entry(exporter_configuration, 1);

    return ddog_prof_Endpoint_agent(char_slice_from_ruby_string(base_url));
  } else {
    rb_raise(rb_eArgError, "Failed to initialize transport: Unexpected working mode, expected :agentless or :agent");
  }
}

static ddog_prof_Exporter_NewResult create_exporter(VALUE exporter_configuration, VALUE tags_as_array) {
  ENFORCE_TYPE(exporter_configuration, T_ARRAY);
  ENFORCE_TYPE(tags_as_array, T_ARRAY);

  // This needs to be called BEFORE convert_tags since it can raise an exception and thus cause the ddog_Vec_Tag
  // to be leaked.
  ddog_prof_Endpoint endpoint = endpoint_from(exporter_configuration);

  ddog_Vec_Tag tags = convert_tags(tags_as_array);

  ddog_CharSlice library_name = DDOG_CHARSLICE_C("dd-trace-rb");
  ddog_CharSlice library_version = char_slice_from_ruby_string(library_version_string);
  ddog_CharSlice profiling_family = DDOG_CHARSLICE_C("ruby");

  ddog_prof_Exporter_NewResult exporter_result =
    ddog_prof_Exporter_new(library_name, library_version, profiling_family, &tags, endpoint);

  ddog_Vec_Tag_drop(tags);

  return exporter_result;
}

static VALUE handle_exporter_failure(ddog_prof_Exporter_NewResult exporter_result) {
  return exporter_result.tag == DDOG_PROF_EXPORTER_NEW_RESULT_OK ?
    Qnil :
    rb_ary_new_from_args(2, error_symbol, get_error_details_and_drop(&exporter_result.err));
}

// Note: This function handles a bunch of libdatadog dynamically-allocated objects, so it MUST not use any Ruby APIs
// which can raise exceptions, otherwise the objects will be leaked.
static VALUE perform_export(
  ddog_prof_Exporter *exporter,
  ddog_Timespec start,
  ddog_Timespec finish,
  ddog_prof_Exporter_Slice_File files_to_compress_and_export,
  ddog_prof_Exporter_Slice_File files_to_export_unmodified,
  ddog_CharSlice internal_metadata,
  ddog_CharSlice info
) {
  ddog_prof_ProfiledEndpointsStats *endpoints_stats = NULL; // Not in use yet
  ddog_prof_Exporter_Request_BuildResult build_result = ddog_prof_Exporter_Request_build(
    exporter,
    start,
    finish,
    files_to_compress_and_export,
    files_to_export_unmodified,
    /* optional_additional_tags: */ NULL,
    endpoints_stats,
    &internal_metadata,
    &info
  );

  if (build_result.tag == DDOG_PROF_EXPORTER_REQUEST_BUILD_RESULT_ERR) {
    ddog_prof_Exporter_drop(exporter);
    return rb_ary_new_from_args(2, error_symbol, get_error_details_and_drop(&build_result.err));
  }

  ddog_CancellationToken *cancel_token = ddog_CancellationToken_new();

  // We'll release the Global VM Lock while we're calling send, so that the Ruby VM can continue to work while this
  // is pending
  call_exporter_without_gvl_arguments args =
    {.exporter = exporter, .build_result = &build_result, .cancel_token = cancel_token, .send_ran = false};

  // We use rb_thread_call_without_gvl2 instead of rb_thread_call_without_gvl as the gvl2 variant never raises any
  // exceptions.
  //
  // (With rb_thread_call_without_gvl, if someone calls Thread#kill or something like it on the current thread,
  // the exception will be raised without us being able to clean up dynamically-allocated stuff, which would leak.)
  //
  // Instead, we take care of our own exception checking, and delay the exception raising (`rb_jump_tag` call) until
  // after we cleaned up any dynamically-allocated resources.
  //
  // We run rb_thread_call_without_gvl2 in a loop since an "interrupt" may cause it to return before even running
  // our code. In such a case, we retry the call -- unless the interrupt was caused by an exception being pending,
  // and in that case we also give up and break out of the loop.
  int pending_exception = 0;

  while (!args.send_ran && !pending_exception) {
    rb_thread_call_without_gvl2(call_exporter_without_gvl, &args, interrupt_exporter_call, cancel_token);

    // To make sure we don't leak memory, we never check for pending exceptions if send ran
    if (!args.send_ran) pending_exception = check_if_pending_exception();
  }

  // Cleanup exporter and token, no longer needed
  ddog_CancellationToken_drop(cancel_token);
  ddog_prof_Exporter_drop(exporter);

  if (pending_exception) {
    // If we got here send did not run, so we need to explicitly dispose of the request
    ddog_prof_Exporter_Request_drop(&build_result.ok);

    // Let Ruby propagate the exception. This will not return.
    rb_jump_tag(pending_exception);
  }

  // The request itself does not need to be freed as libdatadog takes ownership of it as part of sending.

  ddog_prof_Exporter_SendResult result = args.result;

  return result.tag == DDOG_PROF_EXPORTER_SEND_RESULT_HTTP_RESPONSE ?
    rb_ary_new_from_args(2, ok_symbol, UINT2NUM(result.http_response.code)) :
    rb_ary_new_from_args(2, error_symbol, get_error_details_and_drop(&result.err));
}

static VALUE _native_do_export(
  DDTRACE_UNUSED VALUE _self,
  VALUE exporter_configuration,
  VALUE upload_timeout_milliseconds,
  VALUE flush,
  VALUE start_timespec_seconds,
  VALUE start_timespec_nanoseconds,
  VALUE finish_timespec_seconds,
  VALUE finish_timespec_nanoseconds
) {
  VALUE encoded_profile = rb_funcall(flush, rb_intern("encoded_profile"), 0);
  VALUE code_provenance_file_name = rb_funcall(flush, rb_intern("code_provenance_file_name"), 0);
  VALUE code_provenance_data = rb_funcall(flush, rb_intern("code_provenance_data"), 0);
  VALUE tags_as_array = rb_funcall(flush, rb_intern("tags_as_array"), 0);
  VALUE internal_metadata_json = rb_funcall(flush, rb_intern("internal_metadata_json"), 0);
  VALUE info_json = rb_funcall(flush, rb_intern("info_json"), 0);

  ENFORCE_TYPE(upload_timeout_milliseconds, T_FIXNUM);
  ENFORCE_TYPE(start_timespec_seconds, T_FIXNUM);
  ENFORCE_TYPE(start_timespec_nanoseconds, T_FIXNUM);
  ENFORCE_TYPE(finish_timespec_seconds, T_FIXNUM);
  ENFORCE_TYPE(finish_timespec_nanoseconds, T_FIXNUM);
  enforce_encoded_profile_instance(encoded_profile);
  ENFORCE_TYPE(code_provenance_file_name, T_STRING);
  ENFORCE_TYPE(tags_as_array, T_ARRAY);
  ENFORCE_TYPE(internal_metadata_json, T_STRING);
  ENFORCE_TYPE(info_json, T_STRING);

  // Code provenance can be disabled and in that case will be set to nil
  bool have_code_provenance = !NIL_P(code_provenance_data);
  if (have_code_provenance) ENFORCE_TYPE(code_provenance_data, T_STRING);

  uint64_t timeout_milliseconds = NUM2ULONG(upload_timeout_milliseconds);

  ddog_Timespec start =
    {.seconds = NUM2LONG(start_timespec_seconds), .nanoseconds = NUM2UINT(start_timespec_nanoseconds)};
  ddog_Timespec finish =
    {.seconds = NUM2LONG(finish_timespec_seconds), .nanoseconds = NUM2UINT(finish_timespec_nanoseconds)};

  int to_compress_length = have_code_provenance ? 1 : 0;
  ddog_prof_Exporter_File to_compress[to_compress_length];
  int already_compressed_length = 1; // pprof
  ddog_prof_Exporter_File already_compressed[already_compressed_length];

  ddog_prof_Exporter_Slice_File files_to_compress_and_export = {.ptr = to_compress, .len = to_compress_length};
  ddog_prof_Exporter_Slice_File files_to_export_unmodified = {.ptr = already_compressed, .len = already_compressed_length};

  // TODO: Hardcoding the file name will go away with libdatadog 17
  VALUE pprof_file_name = rb_str_new_cstr("rubyprofile.pprof");
  VALUE pprof_data = rb_funcall(encoded_profile, rb_intern("_native_bytes"), 0);
  ENFORCE_TYPE(pprof_data, T_STRING);

  already_compressed[0] = (ddog_prof_Exporter_File) {
    .name = char_slice_from_ruby_string(pprof_file_name),
    .file = byte_slice_from_ruby_string(pprof_data),
  };

  if (have_code_provenance) {
    to_compress[0] = (ddog_prof_Exporter_File) {
      .name = char_slice_from_ruby_string(code_provenance_file_name),
      .file = byte_slice_from_ruby_string(code_provenance_data),
    };
  }

  ddog_CharSlice internal_metadata = char_slice_from_ruby_string(internal_metadata_json);
  ddog_CharSlice info = char_slice_from_ruby_string(info_json);

  ddog_prof_Exporter_NewResult exporter_result = create_exporter(exporter_configuration, tags_as_array);
  // Note: Do not add anything that can raise exceptions after this line, as otherwise the exporter memory will leak

  VALUE failure_tuple = handle_exporter_failure(exporter_result);
  if (!NIL_P(failure_tuple)) return failure_tuple;

  ddog_prof_MaybeError timeout_result = ddog_prof_Exporter_set_timeout(exporter_result.ok, timeout_milliseconds);
  if (timeout_result.tag == DDOG_PROF_OPTION_ERROR_SOME_ERROR) {
    // NOTE: Seems a bit harsh to fail the upload if we can't set a timeout. OTOH, this is only expected to fail
    // if the exporter is not well built. Because such a situation should already be caught above I think it's
    // preferable to leave this here as a virtually unreachable exception rather than ignoring it.
    ddog_prof_Exporter_drop(exporter_result.ok);
    return rb_ary_new_from_args(2, error_symbol, get_error_details_and_drop(&timeout_result.some));
  }

  return perform_export(
    exporter_result.ok,
    start,
    finish,
    files_to_compress_and_export,
    files_to_export_unmodified,
    internal_metadata,
    info
  );
}

static void *call_exporter_without_gvl(void *call_args) {
  call_exporter_without_gvl_arguments *args = (call_exporter_without_gvl_arguments*) call_args;

  args->result = ddog_prof_Exporter_send(args->exporter, &args->build_result->ok, args->cancel_token);
  args->send_ran = true;

  return NULL; // Unused
}

// Called by Ruby when it wants to interrupt call_exporter_without_gvl above, e.g. when the app wants to exit cleanly
static void interrupt_exporter_call(void *cancel_token) {
  ddog_CancellationToken_cancel((ddog_CancellationToken *) cancel_token);
}
