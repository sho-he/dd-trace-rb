require "datadog/profiling"
if Datadog::Profiling.supported?
  require "datadog/profiling/pprof/pprof_pb"
  require "extlz4"
end

module ProfileHelpers
  Sample = Struct.new(:locations, :values, :labels) do |_sample_class| # rubocop:disable Lint/StructNewOverride
    def value?(type)
      (values[type] || 0) > 0
    end

    def has_location?(path:, line:)
      locations.any? do |location|
        location.path == path && location.lineno == line
      end
    end
  end
  Frame = Struct.new(:base_label, :path, :lineno)

  def skip_if_profiling_not_supported(testcase)
    testcase.skip("Profiling is not supported on JRuby") if PlatformHelpers.jruby?
    testcase.skip("Profiling is not supported on TruffleRuby") if PlatformHelpers.truffleruby?

    # Profiling is not officially supported on macOS due to missing libdatadog binaries,
    # but it's still useful to allow it to be enabled for development.
    if PlatformHelpers.mac? && ENV["DD_PROFILING_MACOS_TESTING"] != "true"
      testcase.skip(
        "Profiling is not supported on macOS. If you still want to run these specs, you can use " \
        "DD_PROFILING_MACOS_TESTING=true to override this check."
      )
    end

    return if Datadog::Profiling.supported?

    # Ensure profiling was loaded correctly
    raise "Profiling does not seem to be available: #{Datadog::Profiling.unsupported_reason}. " \
      "Try running `bundle exec rake compile` before running this test."
  end

  def decode_profile(encoded_profile)
    ::Perftools::Profiles::Profile.decode(LZ4.decode(encoded_profile._native_bytes))
  end

  def samples_from_pprof(encoded_profile)
    decoded_profile = decode_profile(encoded_profile)

    string_table = decoded_profile.string_table
    pretty_sample_types = decoded_profile.sample_type.map { |it| string_table[it.type].to_sym }

    decoded_profile.sample.map do |sample|
      Sample.new(
        sample.location_id.map { |location_id| decode_frame_from_pprof(decoded_profile, location_id) },
        pretty_sample_types.zip(sample.value).to_h,
        sample.label.map do |it|
          key = string_table[it.key].to_sym
          [key, ((it.num == 0) ? string_table[it.str] : ProfileHelpers.maybe_fix_label_range(key, it.num))]
        end.sort.to_h,
      ).freeze
    end
  end

  def decode_frame_from_pprof(decoded_profile, location_id)
    strings = decoded_profile.string_table
    location = decoded_profile.location.find { |loc| loc.id == location_id }
    raise "Unexpected: Multiple lines for location" unless location.line.size == 1

    line_entry = location.line.first
    function = decoded_profile.function.find { |func| func.id == line_entry.function_id }

    Frame.new(strings[function.name], strings[function.filename], line_entry.line).freeze
  end

  def object_id_from(thread_id)
    if thread_id != "GC"
      Integer(thread_id.match(/\d+ \((?<object_id>\d+)\)/)[:object_id])
    else
      -1
    end
  end

  def samples_for_thread(samples, thread, expected_size: nil)
    result = samples.select do |sample|
      thread_id = sample.labels[:"thread id"]
      thread_id && object_id_from(thread_id) == thread.object_id
    end

    if expected_size
      expect(result.size).to(be(expected_size), "Found unexpected sample count in result: #{result}")
    end

    result
  end

  def sample_for_thread(samples, thread)
    samples_for_thread(samples, thread, expected_size: 1).first
  end

  def self.maybe_fix_label_range(key, value)
    if [:"local root span id", :"span id"].include?(key) && value < 0
      # pprof labels are defined to be decoded as signed values BUT the backend explicitly interprets these as unsigned
      # 64-bit numbers so we can still use them for these ids without having to fall back to strings
      value + 2**64
    else
      value
    end
  end

  def skip_if_gvl_profiling_not_supported(testcase)
    if RUBY_VERSION < "3.2."
      testcase.skip "GVL profiling is only supported on Ruby >= 3.2"
    end
  end
end

RSpec.configure do |config|
  config.include ProfileHelpers
end
