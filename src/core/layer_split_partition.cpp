#include "core/layer_split_partition.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sstream>
#include <unordered_set>
#include <utility>

#include "core/util.h"

namespace sd {

    static std::string trim_ratio_str(const std::string& value) {
        size_t begin = 0;
        while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
            ++begin;
        }
        size_t end = value.size();
        while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
            --end;
        }
        return value.substr(begin, end - begin);
    }

    static std::string lower_ratio_str(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    }

    static std::vector<std::string> split_ratio_str(const std::string& value, char delimiter) {
        std::vector<std::string> parts;
        std::string part;
        std::istringstream stream(value);
        while (std::getline(stream, part, delimiter)) {
            parts.push_back(part);
        }
        return parts;
    }

    static std::string normalize_ratio_device_key(const std::string& name) {
        std::string s = lower_ratio_str(trim_ratio_str(name));
        s.erase(std::remove(s.begin(), s.end(), '-'), s.end());
        s.erase(std::remove(s.begin(), s.end(), '_'), s.end());
        s.erase(std::remove(s.begin(), s.end(), ':'), s.end());
        return s;
    }

    static bool is_ratio_nan_or_inf(const std::string& s) {
        std::string l = lower_ratio_str(trim_ratio_str(s));
        return l == "nan" || l == "-nan" || l == "+nan" ||
               l == "inf" || l == "-inf" || l == "+inf" ||
               l == "infinity" || l == "-infinity" || l == "+infinity";
    }

    static bool parse_ratio_float_val(const std::string& text, float* out) {
        std::string s = trim_ratio_str(text);
        if (s.empty()) {
            return false;
        }
        char* end = nullptr;
        float v = std::strtof(s.c_str(), &end);
        if (end == s.c_str() || (end != nullptr && *end != '\0')) {
            return false;
        }
        if (!std::isfinite(v) || v < 0.0f) {
            return false;
        }
        *out = v;
        return true;
    }

    static bool parse_single_ratio_spec(const std::string& raw_text, SDSplitRatioSpec* spec, std::string* error) {
        std::string text = trim_ratio_str(raw_text);
        if (text.empty()) {
            return true;
        }

        char delim = '\0';
        if (text.find('&') != std::string::npos) {
            delim = '&';
        } else if (text.find('/') != std::string::npos) {
            delim = '/';
        } else if (text.find(',') != std::string::npos) {
            delim = ',';
        }

        std::vector<std::string> tokens;
        if (delim != '\0') {
            tokens = split_ratio_str(text, delim);
        } else {
            size_t colon_pos = text.find(':');
            if (colon_pos != std::string::npos) {
                std::vector<std::string> colon_parts = split_ratio_str(text, ':');
                bool all_nums = true;
                for (const auto& cp : colon_parts) {
                    float f = 0.0f;
                    if (!parse_ratio_float_val(cp, &f)) {
                        all_nums = false;
                        break;
                    }
                }
                if (all_nums && colon_parts.size() >= 2) {
                    for (const auto& cp : colon_parts) {
                        float f = 0.0f;
                        parse_ratio_float_val(cp, &f);
                        spec->positional_ratios.push_back(f);
                    }
                    return true;
                }
            }
            tokens.push_back(text);
        }

        for (const auto& raw_token : tokens) {
            std::string token = trim_ratio_str(raw_token);
            if (token.empty()) {
                continue;
            }

            size_t sep_pos = token.find('=');
            if (sep_pos == std::string::npos) {
                sep_pos = token.rfind(':');
            }

            if (sep_pos != std::string::npos) {
                std::string key = trim_ratio_str(token.substr(0, sep_pos));
                std::string val_str = trim_ratio_str(token.substr(sep_pos + 1));
                if (is_ratio_nan_or_inf(key) || is_ratio_nan_or_inf(val_str)) {
                    if (error != nullptr) {
                        *error = "invalid split ratio value in '" + token + "'";
                    }
                    return false;
                }
                float f = 0.0f;
                float left_f = 0.0f;
                if (parse_ratio_float_val(key, &left_f) && parse_ratio_float_val(val_str, &f)) {
                    spec->positional_ratios.push_back(left_f);
                    spec->positional_ratios.push_back(f);
                } else if (!parse_ratio_float_val(val_str, &f)) {
                    if (error != nullptr) {
                        *error = "invalid split ratio value in '" + token + "'";
                    }
                    return false;
                } else if (!key.empty() && std::isalpha(static_cast<unsigned char>(key[0]))) {
                    std::string norm_key = normalize_ratio_device_key(key);
                    spec->device_ratios[norm_key] = f;
                } else {
                    if (error != nullptr) {
                        *error = "invalid device or ratio target '" + key + "' in '" + token + "'";
                    }
                    return false;
                }
            } else {
                float f = 0.0f;
                if (!parse_ratio_float_val(token, &f)) {
                    if (error != nullptr) {
                        *error = "invalid split ratio value '" + token + "'";
                    }
                    return false;
                }
                spec->positional_ratios.push_back(f);
            }
        }
        return true;
    }

    bool SDSplitRatioAssignment::parse(const std::string& raw_spec, std::string* error) {
        *this = {};
        std::string in = trim_ratio_str(raw_spec);
        if (in.empty()) {
            return true;
        }

        std::vector<std::string> comma_parts = split_ratio_str(in, ',');
        bool has_equal = false;
        for (const auto& part : comma_parts) {
            std::string p = trim_ratio_str(part);
            if (p.find('=') != std::string::npos) {
                has_equal = true;
                break;
            }
        }

        if (has_equal) {
            for (const auto& part : comma_parts) {
                std::string p = trim_ratio_str(part);
                if (p.empty()) {
                    continue;
                }
                size_t eq = p.find('=');
                if (eq == std::string::npos) {
                    if (!parse_single_ratio_spec(p, &default_spec, error)) {
                        return false;
                    }
                    continue;
                }
                std::string key = lower_ratio_str(trim_ratio_str(p.substr(0, eq)));
                std::string val = trim_ratio_str(p.substr(eq + 1));
                if (val.empty()) {
                    if (error != nullptr) {
                        *error = "missing split ratio value for '" + key + "'";
                    }
                    return false;
                }
                if (key == "all" || key == "default" || key == "*") {
                    if (!parse_single_ratio_spec(val, &default_spec, error)) {
                        return false;
                    }
                } else {
                    SDBackendModule mod;
                    if (!sd_parse_backend_module(key, &mod)) {
                        if (error != nullptr) {
                            *error = "unknown backend module '" + key + "' in split ratio";
                        }
                        return false;
                    }
                    if (!parse_single_ratio_spec(val, &module_specs[mod], error)) {
                        return false;
                    }
                }
            }
        } else {
            if (!parse_single_ratio_spec(in, &default_spec, error)) {
                return false;
            }
        }
        return true;
    }

    bool SDSplitRatioAssignment::ratios_for_backends(SDBackendModule module,
                                                     const std::vector<ggml_backend_t>& backends,
                                                     std::vector<float>* out,
                                                     std::string* error) const {
        if (out == nullptr) {
            return false;
        }
        out->clear();
        if (backends.empty()) {
            return true;
        }

        auto it = module_specs.find(module);
        const SDSplitRatioSpec* spec = (it != module_specs.end() && !it->second.empty()) ? &it->second : &default_spec;
        if (spec->empty()) {
            return true;
        }

        std::vector<float> result(backends.size(), 0.0f);
        auto has_positive = [](const std::vector<float>& ratios) {
            for (float r : ratios) {
                if (r > 0.0f) {
                    return true;
                }
            }
            return false;
        };

        if (!spec->device_ratios.empty()) {
            std::unordered_set<std::string> matched_keys;
            for (size_t i = 0; i < backends.size(); i++) {
                std::string dev_name = layer_split_backend_device_display_name(backends[i]);
                std::string norm1    = normalize_ratio_device_key(dev_name);
                std::string resolved = sd_backend_resolve_name(dev_name);
                std::string norm2    = normalize_ratio_device_key(resolved);

                auto dit = spec->device_ratios.find(norm1);
                if (dit == spec->device_ratios.end() && !norm2.empty() && norm2 != norm1) {
                    dit = spec->device_ratios.find(norm2);
                }
                if (dit != spec->device_ratios.end()) {
                    result[i] = dit->second;
                    matched_keys.insert(dit->first);
                }
            }
            for (const auto& kv : spec->device_ratios) {
                if (matched_keys.count(kv.first) == 0) {
                    if (error != nullptr) {
                        *error = "unknown split-ratio device '" + kv.first + "'";
                    }
                    return false;
                }
            }
            if (!has_positive(result)) {
                if (error != nullptr) {
                    *error = "split ratio is all zeros";
                }
                return false;
            }
            *out = std::move(result);
            return true;
        }

        if (!spec->positional_ratios.empty()) {
            for (size_t i = 0; i < backends.size() && i < spec->positional_ratios.size(); i++) {
                result[i] = spec->positional_ratios[i];
            }
            if (!has_positive(result)) {
                if (error != nullptr) {
                    *error = "split ratio is all zeros";
                }
                return false;
            }
            *out = std::move(result);
            return true;
        }

        return true;
    }

    static bool layer_split_path_segment_starts_at(const std::string& name, size_t pos) {
        return pos == 0 || name[pos - 1] == '.';
    }

    static bool layer_split_has_path_segment(const std::string& name, const char* segment) {
        size_t pos = name.find(segment);
        while (pos != std::string::npos) {
            if (layer_split_path_segment_starts_at(name, pos)) {
                return true;
            }
            pos = name.find(segment, pos + 1);
        }
        return false;
    }

    int layer_split_tensor_block_index(const std::string& name) {
        static const char* unet_block_segments[] = {"input_blocks.", "output_blocks.", "middle_block.",
                                                    "down_blocks.", "up_blocks.", "mid_block."};
        for (const char* segment : unet_block_segments) {
            if (layer_split_has_path_segment(name, segment)) {
                return -1;
            }
        }

        static const char* block_keywords[] = {"transformer_blocks.", "joint_blocks.", "double_blocks.",
                                               "single_blocks.", "blocks.", "block.", "layers."};
        for (const char* keyword : block_keywords) {
            size_t pos = name.find(keyword);
            while (pos != std::string::npos) {
                if (!layer_split_path_segment_starts_at(name, pos)) {
                    pos = name.find(keyword, pos + 1);
                    continue;
                }
                pos += std::strlen(keyword);
                size_t end = pos;
                while (end < name.size() && name[end] >= '0' && name[end] <= '9') {
                    end++;
                }
                if (end > pos && (end == name.size() || name[end] == '.')) {
                    return std::atoi(name.substr(pos, end - pos).c_str());
                }
                break;
            }
        }
        return -1;
    }

    std::string layer_split_backend_device_display_name(ggml_backend_t backend) {
        ggml_backend_dev_t dev = ggml_backend_get_device(backend);
        const char* name       = dev != nullptr ? ggml_backend_dev_name(dev) : ggml_backend_name(backend);
        return name != nullptr ? name : "unknown";
    }

    static size_t graph_cut_layer_split_backend_vram_limit(const std::vector<size_t>& backend_vram_limits,
                                                           size_t backend_index,
                                                           size_t primary_backend_vram_limit) {
        if (backend_index < backend_vram_limits.size()) {
            return backend_vram_limits[backend_index];
        }
        return backend_index == 0 ? primary_backend_vram_limit : 0;
    }

    static std::vector<int64_t> graph_cut_layer_split_backend_capacities(const std::vector<ggml_backend_t>& backends,
                                                                         const std::vector<size_t>& backend_vram_limits,
                                                                         size_t primary_backend_vram_limit) {
        std::vector<int64_t> capacities(backends.size(), std::numeric_limits<int64_t>::max() / 4);
        constexpr int64_t compute_headroom_bytes = 2ll * 1024 * 1024 * 1024;
        for (size_t i = 0; i < backends.size(); i++) {
            ggml_backend_dev_t dev = ggml_backend_get_device(backends[i]);
            size_t free_bytes = 0, total_bytes = 0;
            if (dev != nullptr) {
                ggml_backend_dev_memory(dev, &free_bytes, &total_bytes);
            }
            if (free_bytes > 0) {
                capacities[i] = std::max<int64_t>((int64_t)free_bytes - compute_headroom_bytes, 0);
            }
            size_t limit_bytes = graph_cut_layer_split_backend_vram_limit(backend_vram_limits,
                                                                          i,
                                                                          primary_backend_vram_limit);
            if (limit_bytes > 0) {
                capacities[i] = std::min<int64_t>(capacities[i], (int64_t)limit_bytes);
            }
        }
        return capacities;
    }

    bool partition_graph_cut_layer_split(const char* desc,
                                         ggml_cgraph* gf,
                                         const sd::ggml_graph_cut::Plan& plan,
                                         const std::vector<ggml_backend_t>& split_backends,
                                         const std::vector<size_t>& backend_vram_limits,
                                         size_t primary_backend_vram_limit,
                                         const std::vector<float>& split_ratios,
                                         std::unordered_map<const ggml_tensor*, ggml_backend_t>& param_assignments,
                                         const std::function<ggml_tensor*(ggml_tensor*)>& canonical_param_tensor,
                                         GraphCutLayerSplitAssignment* assignment_out) {
        GGML_ASSERT(gf != nullptr);
        GGML_ASSERT(assignment_out != nullptr);
        GGML_ASSERT(canonical_param_tensor != nullptr);
        GGML_ASSERT(!split_backends.empty());

        GraphCutLayerSplitAssignment assignment;
        assignment.segment_count = plan.segments.size();
        assignment.tensors_by_backend.resize(split_backends.size());
        assignment.bytes_by_backend.resize(split_backends.size(), 0);
        assignment.first_segment_by_backend.resize(split_backends.size(), plan.segments.size());
        assignment.last_segment_by_backend.resize(split_backends.size(), 0);

        std::vector<std::vector<ggml_tensor*>> segment_params(plan.segments.size());
        std::vector<int64_t> segment_param_bytes(plan.segments.size(), 0);
        std::unordered_set<ggml_tensor*> seen_params;
        for (size_t seg_idx = 0; seg_idx < plan.segments.size(); seg_idx++) {
            std::vector<ggml_tensor*> params = sd::ggml_graph_cut::param_tensors(gf, plan.segments[seg_idx]);
            for (ggml_tensor* raw_param : params) {
                ggml_tensor* param = canonical_param_tensor(raw_param);
                if (param == nullptr || !seen_params.insert(param).second) {
                    continue;
                }
                segment_params[seg_idx].push_back(param);
                segment_param_bytes[seg_idx] += (int64_t)ggml_nbytes(param);
            }
        }

        int64_t total_param_bytes = 0;
        for (int64_t bytes : segment_param_bytes) {
            total_param_bytes += bytes;
        }
        if (total_param_bytes <= 0) {
            LOG_ERROR("%s graph-cut layer split found no graph params to assign", desc);
            return false;
        }

        std::vector<int64_t> backend_capacities = graph_cut_layer_split_backend_capacities(split_backends,
                                                                                           backend_vram_limits,
                                                                                           primary_backend_vram_limit);
        // Existing placements may already occupy the reported free VRAM. Reuse
        // them; execution checks missing weights and reclaims memory as needed.
        const bool reuse_assignments = std::all_of(seen_params.begin(), seen_params.end(), [&](ggml_tensor* param) {
            return param_assignments.count(param) != 0;
        });

        std::vector<ggml_backend_t> backend_by_segment(plan.segments.size(), split_backends[0]);
        bool use_ratio_partition = false;
        if (!split_ratios.empty()) {
            for (float r : split_ratios) {
                if (r > 0.0f) {
                    use_ratio_partition = true;
                    break;
                }
            }
        }

        if (!reuse_assignments) {
            if (use_ratio_partition) {
                const size_t K = split_backends.size();
                const size_t M = plan.segments.size();

                std::vector<float> ratios(K, 0.0f);
                float sum_w = 0.0f;
                for (size_t i = 0; i < K; i++) {
                    if (i < split_ratios.size() && split_ratios[i] > 0.0f) {
                        ratios[i] = split_ratios[i];
                        sum_w += ratios[i];
                    }
                }
                if (sum_w <= 0.0f) {
                    sum_w     = 1.0f;
                    ratios[0] = 1.0f;
                }

                std::vector<double> target_bytes(K, 0.0);
                for (size_t i = 0; i < K; i++) {
                    target_bytes[i] = (double)total_param_bytes * (double)ratios[i] / (double)sum_w;
                }

                std::vector<int64_t> pref(M + 1, 0);
                for (size_t s = 0; s < M; s++) {
                    pref[s + 1] = pref[s] + segment_param_bytes[s];
                }

                std::vector<std::vector<double>> dp(K + 1, std::vector<double>(M + 1, std::numeric_limits<double>::infinity()));
                std::vector<std::vector<size_t>> parent(K + 1, std::vector<size_t>(M + 1, 0));
                dp[0][0] = 0.0;

                for (size_t i = 0; i < K; i++) {
                    for (size_t a = 0; a <= M; a++) {
                        if (!std::isfinite(dp[i][a])) {
                            continue;
                        }
                        for (size_t b = a; b <= M; b++) {
                            int64_t bytes = pref[b] - pref[a];
                            if (bytes > backend_capacities[i]) {
                                break;
                            }
                            double diff_mb = ((double)bytes - target_bytes[i]) / (1024.0 * 1024.0);
                            double cost    = dp[i][a] + diff_mb * diff_mb;
                            if (cost < dp[i + 1][b]) {
                                dp[i + 1][b]     = cost;
                                parent[i + 1][b] = a;
                            }
                        }
                    }
                }

                if (std::isfinite(dp[K][M])) {
                    size_t curr = M;
                    for (size_t i = K; i >= 1; i--) {
                        size_t prev = parent[i][curr];
                        for (size_t s = prev; s < curr; s++) {
                            backend_by_segment[s] = split_backends[i - 1];
                        }
                        curr = prev;
                    }
                    std::string ratio_str;
                    for (size_t i = 0; i < K; i++) {
                        if (i > 0) {
                            ratio_str += ":";
                        }
                        char buf[32];
                        snprintf(buf, sizeof(buf), "%.2g", ratios[i]);
                        ratio_str += buf;
                    }
                    LOG_INFO("%s graph-cut layer split: applied split ratio %s", desc, ratio_str.c_str());
                } else {
                    LOG_ERROR("%s graph-cut layer split: requested split ratio cannot be satisfied within device VRAM capacities", desc);
                    return false;
                }
            }

            if (!use_ratio_partition) {
                size_t current_backend = 0;
                int64_t current_used   = 0;
                for (size_t seg_idx = 0; seg_idx < plan.segments.size(); seg_idx++) {
                    int64_t bytes = segment_param_bytes[seg_idx];
                    while (current_backend + 1 < split_backends.size() &&
                           bytes > 0 &&
                           current_used + bytes > backend_capacities[current_backend]) {
                        current_backend++;
                        current_used = 0;
                    }
                    if (bytes > 0 && current_used + bytes > backend_capacities[current_backend]) {
                        LOG_ERROR("%s graph-cut layer split: segment %zu needs %.1f MB on %s, but only %.1f MB is available under current VRAM limits",
                                  desc,
                                  seg_idx,
                                  (current_used + bytes) / (1024.0 * 1024.0),
                                  layer_split_backend_device_display_name(split_backends[current_backend]).c_str(),
                                  backend_capacities[current_backend] / (1024.0 * 1024.0));
                        return false;
                    }
                    current_used += bytes;
                    backend_by_segment[seg_idx] = split_backends[current_backend];
                }
            }
        }

        for (size_t seg_idx = 0; seg_idx < plan.segments.size(); seg_idx++) {
            for (ggml_tensor* param : segment_params[seg_idx]) {
                ggml_backend_t target_backend = backend_by_segment[seg_idx];
                auto assigned_it              = param_assignments.find(param);
                if (assigned_it == param_assignments.end()) {
                    param_assignments[param]            = target_backend;
                    assignment.has_new_param_assignment = true;
                } else {
                    target_backend = assigned_it->second;
                    backend_by_segment[seg_idx] = target_backend;
                }

                auto backend_it = std::find(split_backends.begin(), split_backends.end(), target_backend);
                if (backend_it == split_backends.end()) {
                    LOG_ERROR("%s graph-cut layer split tensor '%s' is assigned to an unavailable backend",
                              desc,
                              ggml_get_name(param));
                    return false;
                }
                size_t backend_idx = (size_t)std::distance(split_backends.begin(), backend_it);
                assignment.first_segment_by_backend[backend_idx] = std::min(assignment.first_segment_by_backend[backend_idx], seg_idx);
                assignment.last_segment_by_backend[backend_idx]  = std::max(assignment.last_segment_by_backend[backend_idx], seg_idx + 1);
                assignment.tensors_by_backend[backend_idx].push_back(param);
                assignment.bytes_by_backend[backend_idx] += (int64_t)ggml_nbytes(param);
            }
        }

        const int n_nodes = ggml_graph_n_nodes(gf);
        for (size_t seg_idx = 0; seg_idx < plan.segments.size(); seg_idx++) {
            ggml_backend_t backend = backend_by_segment[seg_idx];
            const auto& segment    = plan.segments[seg_idx];
            for (int node_index : segment.internal_node_indices) {
                if (node_index < 0 || node_index >= n_nodes) {
                    continue;
                }
                ggml_tensor* node = ggml_graph_node(gf, node_index);
                if (node != nullptr) {
                    assignment.node_assignments[node] = backend;
                }
            }
            for (int node_index : segment.output_node_indices) {
                if (node_index < 0 || node_index >= n_nodes) {
                    continue;
                }
                ggml_tensor* node = ggml_graph_node(gf, node_index);
                if (node != nullptr) {
                    assignment.node_assignments[node] = backend;
                }
            }
        }

        *assignment_out = std::move(assignment);
        return true;
    }

    void log_graph_cut_layer_split_assignment(const char* desc,
                                              const std::vector<ggml_backend_t>& split_backends,
                                              const GraphCutLayerSplitAssignment& assignment) {
        for (size_t i = 0; i < split_backends.size(); i++) {
            if (i >= assignment.tensors_by_backend.size() ||
                assignment.tensors_by_backend[i].empty()) {
                continue;
            }
            size_t first_segment = assignment.first_segment_by_backend[i] == assignment.segment_count
                                       ? 0
                                       : assignment.first_segment_by_backend[i];
            size_t last_segment  = assignment.last_segment_by_backend[i];
            if (assignment.has_new_param_assignment) {
                LOG_INFO("%s graph-cut layer split: %s <- segments [%zu, %zu), %zu tensors, %.1f MB",
                         desc,
                         layer_split_backend_device_display_name(split_backends[i]).c_str(),
                         first_segment,
                         last_segment,
                         assignment.tensors_by_backend[i].size(),
                         assignment.bytes_by_backend[i] / (1024.0 * 1024.0));
            } else {
                LOG_VERBOSE("%s graph-cut layer split: %s <- segments [%zu, %zu), %zu tensors, %.1f MB",
                            desc,
                            layer_split_backend_device_display_name(split_backends[i]).c_str(),
                            first_segment,
                            last_segment,
                            assignment.tensors_by_backend[i].size(),
                            assignment.bytes_by_backend[i] / (1024.0 * 1024.0));
            }
        }
    }

}  // namespace sd
