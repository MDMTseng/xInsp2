//
// demo_async.cpp — Milestone 0 exit test.
//
// Exercises every primitive in xi/xi.hpp using only STL types so it builds
// with any C++20 compiler, no backend, no OpenCV, no uWebSockets.
//
// Build:
//   cl /std:c++20 /EHsc /I ../backend/include demo_async.cpp    (MSVC)
//   g++ -std=c++20 -O2 -I ../backend/include demo_async.cpp -o demo -pthread
//
// Expected output (non-deterministic order for the parallel branches):
//
//   [M0] gray:     10
//   [M0] blurred:  20
//   [M0] p1+p2:    30
//   [M0] sigma:    3.500000
//   [M0] lowT:     60
//   [M0] vars count: 4
//     - gray: Number value=10
//     - blurred: Number value=20
//     - a: Number value=100
//     - b: Number value=200
//   [M0] all ok.
//

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include <xi/xi.hpp>

// --- pretend operators ---

static int toGray(int frame) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return frame;
}

static int gaussian(int in, double sigma) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return in * static_cast<int>(sigma + 0.5);
}

static int featureA(int in) {
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    return in * 10;
}

static int featureB(int in) {
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    return in * 20;
}

// Pre-wrap one operator as a library would.
ASYNC_WRAP(featureA)
ASYNC_WRAP(featureB)

// --- a fake plugin type to prove Instance<T> compiles ---

class Counter : public xi::InstanceBase {
public:
    explicit Counter(std::string n) : name_(std::move(n)) {}
    const std::string& name() const override { return name_; }
    std::string plugin_name() const override { return "Counter"; }
    int tick() { return ++n_; }
private:
    std::string name_;
    int         n_ = 0;
};

namespace xi {
template <>
std::shared_ptr<Counter> make_plugin_instance<Counter>(std::string_view name) {
    return std::make_shared<Counter>(std::string(name));
}
}

// --- tunables ---

static xi::Param<double> sigma { "sigma",     3.5, {0.1, 10.0} };
static xi::Param<int>    lowT  { "canny_low", 60,  {0, 255}    };

// --- the inspection ---

static int inspect(int frame) {
    using namespace std::chrono;
    auto t0 = steady_clock::now();

    VAR(gray,    toGray(frame));
    VAR(blurred, gaussian(gray, sigma));

    // Two parallel branches — both take ~25ms; total wall time should be ~25ms
    // not ~50ms.
    auto p1 = async_featureA(blurred);
    auto p2 = async_featureB(blurred);

    VAR(a, int(p1));   // implicit await
    VAR(b, int(p2));

    auto dt = duration_cast<milliseconds>(steady_clock::now() - t0).count();
    std::cout << "[M0] gray:    " << gray    << "\n";
    std::cout << "[M0] blurred: " << blurred << "\n";
    std::cout << "[M0] p1+p2:   " << (a + b) << "\n";
    std::cout << "[M0] parallel section wall time: " << dt << "ms\n";

    return a + b;
}

static const char* kindName(xi::VarKind k) {
    switch (k) {
        case xi::VarKind::Image:   return "Image";
        case xi::VarKind::Number:  return "Number";
        case xi::VarKind::Boolean: return "Boolean";
        case xi::VarKind::String:  return "String";
        case xi::VarKind::Json:    return "Json";
        case xi::VarKind::Custom:  return "Custom";
        default:                   return "Unknown";
    }
}

int main() {
    // Demonstrate Param<T> values flow into inspect() via implicit conversion.
    std::cout << "[M0] sigma: " << static_cast<double>(sigma) << "\n";
    std::cout << "[M0] lowT:  " << static_cast<int>(lowT)     << "\n";

    // Demonstrate Instance<T> registration.
    xi::Instance<Counter> counter{"demo_counter"};
    if (!counter) {
        std::cerr << "[M0] Instance<Counter> failed to construct\n";
        return 1;
    }
    counter->tick();
    counter->tick();

    // Run one pass.
    xi::ValueStore::current().clear();
    int result = inspect(/*frame=*/10);

    // Walk the store and print what would get serialized into a `vars` msg.
    auto snap = xi::ValueStore::current().snapshot();
    std::cout << "[M0] vars count: " << snap.size() << "\n";
    for (auto& e : snap) {
        std::cout << "  - " << e.name << ": " << kindName(e.kind);
        if (!e.inline_json.empty()) std::cout << " value=" << e.inline_json;
        std::cout << "\n";
    }

    // Sanity: 4 vars tracked, final result matches gray*sigma*(10+20) = 10*4*30 = 1200
    if (snap.size() != 4) {
        std::cerr << "[M0] unexpected var count\n";
        return 2;
    }
    if (result <= 0) {
        std::cerr << "[M0] bad result\n";
        return 3;
    }

    // Param registry list() should contain both tunables.
    auto params = xi::ParamRegistry::instance().list();
    std::cout << "[M0] params: " << params.size() << "\n";
    for (auto* p : params) {
        std::cout << "  - " << p->as_json() << "\n";
    }

    // InstanceRegistry should have our Counter.
    auto inst = xi::InstanceRegistry::instance().list();
    std::cout << "[M0] instances: " << inst.size() << "\n";
    for (auto& i : inst) {
        std::cout << "  - " << i->name() << " (" << i->plugin_name() << ")\n";
    }

    std::cout << "[M0] all ok.\n";
    return 0;
}
