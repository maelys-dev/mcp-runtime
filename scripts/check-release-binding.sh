#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <engineering-projectctl.toml>" >&2
  exit 2
fi

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
ruby -rpsych - "$1" "$root/.github/workflows/ci.yml" <<'RUBY'
config_path, ci_path = ARGV

def fail!(message)
  abort("release binding: #{message}")
end

def decode(node)
  case node
  when Psych::Nodes::Scalar
    fail!("anchors and tags are not allowed") if node.anchor || node.tag
    node.value
  when Psych::Nodes::Sequence
    fail!("flow sequences are not allowed") unless node.style == Psych::Nodes::Sequence::BLOCK
    fail!("anchors and tags are not allowed") if node.anchor || node.tag
    node.children.map { |child| decode(child) }
  when Psych::Nodes::Mapping
    fail!("flow mappings are not allowed") unless node.style == Psych::Nodes::Mapping::BLOCK
    fail!("anchors and tags are not allowed") if node.anchor || node.tag
    fail!("mapping has an odd number of nodes") unless node.children.length.even?
    value = {}
    node.children.each_slice(2) do |key, child|
      fail!("mapping keys must be plain scalars") unless key.is_a?(Psych::Nodes::Scalar)
      decoded_key = decode(key)
      fail!("duplicate mapping key #{decoded_key.inspect}") if value.key?(decoded_key)
      value[decoded_key] = decode(child)
    end
    value
  when Psych::Nodes::Alias
    fail!("aliases are not allowed")
  else
    fail!("unexpected YAML node #{node.class}")
  end
end

def load_yaml(path)
  stream = Psych.parse_stream(File.read(path, encoding: "UTF-8"))
  fail!("#{path} must contain exactly one YAML document") unless stream.children.length == 1
  document = stream.children.fetch(0)
  fail!("#{path} document anchors and tags are not allowed") if document.tag || document.implicit == false
  decode(document.root)
end

config_text = File.read(config_path, encoding: "UTF-8")
section = config_text.match(/^\[release_transaction\]\n(.*?)(?=^\[|\z)/m)
fail!("engineering configuration has no release_transaction binding") unless section
binding_text = section[1]

def quoted_value(text, key)
  matches = text.scan(/^#{Regexp.escape(key)} = "([^"]+)"\s*(?:#.*)?$/).flatten
  fail!("release_transaction.#{key} must occur exactly once") unless matches.length == 1
  matches.fetch(0)
end

def string_array(text, key)
  matches = text.scan(/^#{Regexp.escape(key)} = \[(.*?)\]\s*(?:#.*)?$/m).flatten
  fail!("release_transaction.#{key} must occur exactly once") unless matches.length == 1
  body = matches.fetch(0)
  values = body.scan(/"([^"]+)"/).flatten
  remainder = body.gsub(/"[^"]+"|,|\s|#.*$/, "")
  fail!("release_transaction.#{key} must be a string array") unless remainder.empty?
  values
end

expected = {
  "binding" => "external-command",
  "tag_template" => "v{version}",
  "metadata_paths" => ["Makefile", "CHANGELOG.md"],
  "prepare" => ["bash", "scripts/prepare-release.sh", "{version}"],
  "version_probe" => ["bash", "scripts/release-version.sh"],
  "verify" => ["bash", "scripts/check-release-contract.sh"]
}
expected.each do |key, value|
  actual = value.is_a?(Array) ? string_array(binding_text, key) : quoted_value(binding_text, key)
  fail!("release_transaction.#{key} does not match the sealed contract") unless actual == value
end

ci = load_yaml(ci_path)
fail!("ci.yml must define workflow CI") unless ci["name"] == "CI"
jobs = ci["jobs"]
fail!("ci.yml jobs must be a mapping") unless jobs.is_a?(Hash)
expected_job_ids = %w[check sanitizers fuzz official-conformance]
fail!("ci.yml must expose exactly the four sealed job keys") unless jobs.keys == expected_job_ids
expected_job_names = [
  "Native, SDK and provider checks",
  "ASan, UBSan and TSan",
  "Fuzzer smoke campaigns",
  "Supported official MCP scenarios"
]
actual_job_names = expected_job_ids.map do |job_id|
  job = jobs.fetch(job_id)
  fail!("ci.yml job #{job_id} must be a mapping") unless job.is_a?(Hash)
  job["name"]
end
fail!("ci.yml must expose the four sealed CI job names") unless actual_job_names == expected_job_names

check_steps = jobs.fetch("check")["steps"]
fail!("ci.yml check job must define steps") unless check_steps.is_a?(Array)
release_guard_steps = check_steps.select { |step| step.is_a?(Hash) && step["run"] == "make test-release-contract" }
fail!("ci.yml check job must run make test-release-contract exactly once") unless release_guard_steps.length == 1

fail!("release_transaction.expected_workflow must be CI") unless quoted_value(binding_text, "expected_workflow") == "CI"
bound_jobs = string_array(binding_text, "expected_jobs")
fail!("release_transaction.expected_jobs must be the four sealed CI jobs") unless bound_jobs == expected_job_names
fail!("release_transaction.expected_jobs diverges from ci.yml") unless bound_jobs == actual_job_names
RUBY
