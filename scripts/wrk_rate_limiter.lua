math.randomseed(os.time())

local client_pool_size = tonumber(os.getenv("CLIENT_POOL_SIZE") or "250")
local allowed = 0
local rejected = 0
local other = 0

request = function()
  local client_id = "user" .. tostring(math.random(client_pool_size))
  local path = "/request?client_id=" .. client_id
  return wrk.format("GET", path)
end

response = function(status, headers, body)
  if status == 200 then
    allowed = allowed + 1
  elseif status == 429 then
    rejected = rejected + 1
  else
    other = other + 1
  end
end

done = function(summary, latency, requests)
  io.write("\nLoad test summary\n")
  io.write("throughput_req_per_sec=" .. string.format("%.2f", summary.requests / summary.duration * 1000000) .. "\n")
  io.write("avg_latency_ms=" .. string.format("%.2f", latency.mean / 1000) .. "\n")
  io.write("p95_latency_ms=" .. string.format("%.2f", latency:percentile(95.0) / 1000) .. "\n")
  io.write("allowed_requests=" .. allowed .. "\n")
  io.write("rejected_requests=" .. rejected .. "\n")
  io.write("other_statuses=" .. other .. "\n")

  local total_accounted = allowed + rejected
  if total_accounted > 0 then
    io.write("rate_limit_accuracy=" .. string.format("%.4f", total_accounted / summary.requests) .. "\n")
  else
    io.write("rate_limit_accuracy=0.0000\n")
  end
end
