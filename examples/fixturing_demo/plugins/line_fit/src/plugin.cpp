//
// line_fit.cpp — pose-aligned search line (fixturing demo).
//
// The search line is authored once in a BASELINE pose frame (stored as config).
// At runtime the plugin receives the part's CURRENT pose and transforms the line
// by the rigid delta T = current ∘ baseline⁻¹, then "fits" (here: just emits the
// transformed line). The transform is the plugin's own internal job — the
// framework only delivers `current`.
//
// process(input):
//   input "current" : {x, y, angle}  REQUIRED — if missing → NA (empty + reason)
//   returns:
//     "line" : {x1, y1, x2, y2}       the search line at the current pose
//     "current" : echoed pose
//
// Demonstrates: xi::require() guarding the compute boundary; an NA result that
// propagates downstream. See docs/design/io-types-and-na.md.
//
#include <xi/xi_json.hpp>

#include <cmath>

class LineFit : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    xi::Record process(const xi::Record& input) override {
        // Validation at the compute boundary: no current pose → NA, with reason.
        if (auto na = xi::require(input, {"current"})) return *na;

        const auto cur = input["current"];
        double cx = cur["x"].as_double(0), cy = cur["y"].as_double(0);
        double ca = cur["angle"].as_double(0);

        // Rigid delta from baseline to current (angle in degrees).
        const double deg = 3.14159265358979323846 / 180.0;
        double dth = (ca - baseline_angle_) * deg;
        double dx  = cx - baseline_x_, dy = cy - baseline_y_;
        double s = std::sin(dth), c = std::cos(dth);

        // Map a baseline-frame point to the current pose: translate by the pose
        // delta, then rotate about the current position.
        auto xform = [&](double x, double y, double& ox, double& oy) {
            double tx = x + dx - cx, ty = y + dy - cy;
            ox = cx + tx * c - ty * s;
            oy = cy + tx * s + ty * c;
        };

        double ox1, oy1, ox2, oy2;
        xform(line_x1_, line_y1_, ox1, oy1);
        xform(line_x2_, line_y2_, ox2, oy2);

        xi::Record out;
        out.set("line", xi::Record().set("x1", ox1).set("y1", oy1).set("x2", ox2).set("y2", oy2));
        out.set("current", xi::Record().set("x", cx).set("y", cy).set("angle", ca));
        return out;
    }

    std::string exchange(const std::string& cmd) override {
        auto p = xi::Json::parse(cmd);
        auto command = p["command"].as_string();
        if (command == "set_baseline") {
            baseline_x_     = p["x"].as_double(baseline_x_);
            baseline_y_     = p["y"].as_double(baseline_y_);
            baseline_angle_ = p["angle"].as_double(baseline_angle_);
        } else if (command == "set_line") {
            line_x1_ = p["x1"].as_double(line_x1_);  line_y1_ = p["y1"].as_double(line_y1_);
            line_x2_ = p["x2"].as_double(line_x2_);  line_y2_ = p["y2"].as_double(line_y2_);
        }
        return get_def();
    }

    std::string get_def() const override {
        return xi::Json::object()
            .set("baseline_x", baseline_x_).set("baseline_y", baseline_y_).set("baseline_angle", baseline_angle_)
            .set("line_x1", line_x1_).set("line_y1", line_y1_).set("line_x2", line_x2_).set("line_y2", line_y2_)
            .dump();
    }

    bool set_def(const std::string& json) override {
        auto p = xi::Json::parse(json);
        if (!p.valid()) return false;
        baseline_x_     = p["baseline_x"].as_double(baseline_x_);
        baseline_y_     = p["baseline_y"].as_double(baseline_y_);
        baseline_angle_ = p["baseline_angle"].as_double(baseline_angle_);
        line_x1_ = p["line_x1"].as_double(line_x1_);  line_y1_ = p["line_y1"].as_double(line_y1_);
        line_x2_ = p["line_x2"].as_double(line_x2_);  line_y2_ = p["line_y2"].as_double(line_y2_);
        return true;
    }

private:
    double baseline_x_ = 0, baseline_y_ = 0, baseline_angle_ = 0;
    double line_x1_ = 10, line_y1_ = 0, line_x2_ = 10, line_y2_ = 40;
};

XI_PLUGIN_IMPL(LineFit)
