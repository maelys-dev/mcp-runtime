#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
workflow="$root/.github/workflows/release.yml"

ruby -rpsych - "$workflow" <<'RUBY'
path = ARGV.fetch(0)

def fail!(message)
  abort("release workflow: #{message}")
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

stream = Psych.parse_stream(File.read(path, encoding: "UTF-8"))
fail!("must contain exactly one YAML document") unless stream.children.length == 1
document = stream.children.fetch(0)
fail!("document anchors and tags are not allowed") if document.tag || document.implicit == false
root = decode(document.root)

def require_keys!(value, expected, label)
  actual = value.keys.sort
  wanted = expected.sort
  fail!("#{label} keys must be #{wanted.join(', ')}") unless actual == wanted
end

def require_value!(actual, expected, label)
  fail!("#{label} must be #{expected.inspect}") unless actual == expected
end

checkout = "actions/checkout@fbc6f3992d24b796d5a048ff273f7fcc4a7b6c09"
setup_node = "actions/setup-node@a0853c24544627f65ddf259abe73b1d18a591444"
install_native = "sudo apt-get update && sudo apt-get install --yes clang make pkg-config libjansson-dev liburiparser-dev python3"
tag_validation = <<~SH
  test "$GITHUB_REF_TYPE" = "tag"
  version="$(bash scripts/release-version.sh)"
  test "$GITHUB_REF_NAME" = "v$version"
  bash scripts/check-release-contract.sh
SH
publish_release = <<~SH
  version="$(bash scripts/release-version.sh)"
  gh release create "$GITHUB_REF_NAME" --title "mcp-runtime $version" --generate-notes --verify-tag
SH

require_keys!(root, %w[name on permissions jobs], "top-level")
require_value!(root.fetch("name"), "Release", "name")
require_value!(root.fetch("permissions"), { "contents" => "read" }, "default permissions")

trigger = root.fetch("on")
require_keys!(trigger, ["push"], "trigger")
push = trigger.fetch("push")
require_keys!(push, ["tags"], "push trigger")
require_value!(push.fetch("tags"), ["v*"], "tag trigger")

jobs = root.fetch("jobs")
require_keys!(jobs, %w[verify publish], "jobs")

verify = jobs.fetch("verify")
require_keys!(verify, %w[name runs-on timeout-minutes steps], "verify")
require_value!(verify.fetch("name"), "Verify tagged release", "verify name")
require_value!(verify.fetch("runs-on"), "ubuntu-24.04", "verify runner")
require_value!(verify.fetch("timeout-minutes"), "20", "verify timeout")
expected_verify_steps = [
  { "uses" => checkout },
  {
    "uses" => setup_node,
    "with" => { "node-version" => "24", "package-manager-cache" => "false" }
  },
  { "name" => "Install native dependencies", "run" => install_native },
  { "name" => "Verify tag and release metadata", "run" => tag_validation },
  { "run" => "make check-all CC=clang ANALYZER=clang" }
]
require_value!(verify.fetch("steps"), expected_verify_steps, "verify steps")

publish = jobs.fetch("publish")
require_keys!(publish, %w[name needs runs-on timeout-minutes permissions steps], "publish")
require_value!(publish.fetch("name"), "Publish GitHub release", "publish name")
require_value!(publish.fetch("needs"), "verify", "publish dependency")
require_value!(publish.fetch("runs-on"), "ubuntu-24.04", "publish runner")
require_value!(publish.fetch("timeout-minutes"), "5", "publish timeout")
require_value!(publish.fetch("permissions"), { "contents" => "write" }, "publish permissions")
expected_publish_steps = [
  { "uses" => checkout },
  {
    "name" => "Create GitHub release for the pushed tag",
    "env" => { "GH_TOKEN" => "${{ github.token }}" },
    "run" => publish_release
  }
]
require_value!(publish.fetch("steps"), expected_publish_steps, "publish steps")

run_values = [verify, publish].flat_map { |job| job.fetch("steps") }.map { |step| step["run"] }.compact
release_creates = run_values.grep(/\bgh release create\b/)
require_value!(release_creates, [publish_release], "GitHub release creation")
RUBY
