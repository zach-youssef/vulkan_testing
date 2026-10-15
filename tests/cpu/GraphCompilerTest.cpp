#include "graph/GraphCompiler.h"

#include <doctest/doctest.h>

namespace {

// Builds GraphDescs in tests
struct Builder {
    GraphDesc desc;

    explicit Builder(uint32_t queues = 1) {
        desc.queues.assign(queues, CompilerQueue{});
    }

    uint32_t image(const std::string& name, bool surface = false, bool resting = false) {
        desc.resources.push_back({name, true, surface, false, resting});
        return static_cast<uint32_t>(desc.resources.size() - 1);
    }

    uint32_t buffer(const std::string& name, bool perFrame = false) {
        desc.resources.push_back({name, false, false, perFrame, false});
        return static_cast<uint32_t>(desc.resources.size() - 1);
    }

    uint32_t node(const std::string& name, NodeKind kind, std::vector<CompilerUse> uses = {}) {
        desc.nodes.push_back({name, kind, true, false, std::move(uses)});
        return static_cast<uint32_t>(desc.nodes.size() - 1);
    }

    CompiledGraph compile() { return compileGraph(desc); }
};

std::vector<std::string> names(const GraphDesc& desc, const std::vector<uint32_t>& nodes) {
    std::vector<std::string> result;
    for (uint32_t node : nodes) {
        result.push_back(desc.nodes[node].name);
    }
    return result;
}

} // namespace

TEST_CASE("Graph compiler orders nodes by read-after-write, write-after-read and write-after-write") {
    Builder b;
    uint32_t image = b.image("image");
    // Insertion order is program order: write, read, write again
    b.node("write1", NodeKind::Compute, {{image, Access::StorageWrite}});
    b.node("read", NodeKind::Compute, {{image, Access::StorageRead}});
    b.node("write2", NodeKind::Compute, {{image, Access::StorageWrite}});
    auto compiled = b.compile();
    CHECK(names(b.desc, compiled.order) == std::vector<std::string>{"write1", "read", "write2"});
    CHECK(compiled.predecessors[1] == std::vector<uint32_t>{0});
    // write2 depends on both the read (WAR) and write1 (WAW)
    CHECK(compiled.predecessors[2] == std::vector<uint32_t>{0, 1});
}

TEST_CASE("Graph compiler keeps insertion order for independent nodes and honours explicit dependencies") {
    Builder b;
    b.node("a", NodeKind::Compute);
    b.node("b", NodeKind::Compute);
    b.node("c", NodeKind::Compute);
    CHECK(names(b.desc, b.compile().order) == std::vector<std::string>{"a", "b", "c"});

    b.desc.dependencies.push_back({2, 0});   // c before a
    CHECK(names(b.desc, b.compile().order) == std::vector<std::string>{"b", "c", "a"});
    CHECK(b.compile().explicitPredecessors[0] == std::vector<uint32_t>{2});
}

TEST_CASE("Graph compiler reports cycles from explicit dependencies") {
    Builder b;
    uint32_t image = b.image("image");
    b.node("producer", NodeKind::Compute, {{image, Access::StorageWrite}});
    b.node("consumer", NodeKind::Compute, {{image, Access::SampledRead}});
    b.desc.dependencies.push_back({1, 0});
    CHECK_THROWS_WITH_AS(b.compile(), doctest::Contains("cycle through: producer, consumer"), GraphError);

    b.desc.dependencies = {{0, 0}};
    CHECK_THROWS_AS(b.compile(), GraphError);
}

TEST_CASE("Graph compiler drops disabled nodes and their dependencies") {
    Builder b;
    uint32_t image = b.image("image");
    b.node("write", NodeKind::Compute, {{image, Access::StorageWrite}});
    uint32_t read = b.node("read", NodeKind::Compute, {{image, Access::SampledRead}});
    b.desc.nodes[0].enabled = false;
    auto compiled = b.compile();
    CHECK(compiled.order == std::vector<uint32_t>{read});
    CHECK(compiled.predecessors[read].empty());
    CHECK(compiled.nodeQueue[0] == -1);
}

TEST_CASE("Graph compiler validates accesses against resource kinds and node kinds") {
    {
        Builder b;
        uint32_t buffer = b.buffer("buffer");
        b.node("bad", NodeKind::Compute, {{buffer, Access::SampledRead}});
        CHECK_THROWS_WITH_AS(b.compile(), doctest::Contains("image access SampledRead"), GraphError);
    }
    {
        Builder b;
        uint32_t image = b.image("image");
        b.node("bad", NodeKind::Compute, {{image, Access::UniformRead}});
        CHECK_THROWS_WITH_AS(b.compile(), doctest::Contains("buffer access UniformRead"), GraphError);
    }
    {
        Builder b;
        uint32_t buffer = b.buffer("buffer");
        b.node("cpu", NodeKind::Cpu, {{buffer, Access::StorageBufferRead}});
        CHECK_THROWS_AS(b.compile(), GraphError);
    }
    {
        Builder b;
        uint32_t buffer = b.buffer("buffer");
        b.node("gpu", NodeKind::Compute, {{buffer, Access::HostWrite}});
        CHECK_THROWS_AS(b.compile(), GraphError);
    }
}

TEST_CASE("Graph compiler allows one surface writer and reads after it") {
    Builder b;
    uint32_t surface = b.image("surface", true);
    uint32_t draw = b.node("draw", NodeKind::Graphics, {{surface, Access::ColorAttachment}});
    uint32_t copy = b.node("copy", NodeKind::Transfer, {{surface, Access::TransferSrc}});
    auto compiled = b.compile();
    CHECK(compiled.usesSurface);
    // Surface users run on the primary queue, here in one submission that acquires and presents
    CHECK(compiled.nodeQueue[draw] == 0);
    CHECK(compiled.nodeQueue[copy] == 0);
    REQUIRE(compiled.submissions.size() == 1);
    CHECK(compiled.submissions[0].acquiresSurface);
    CHECK(compiled.submissions[0].presentsSurface);

    b.node("second writer", NodeKind::Graphics, {{surface, Access::ColorAttachment}});
    CHECK_THROWS_WITH_AS(b.compile(), doctest::Contains("More than one node writes the surface"), GraphError);
}

TEST_CASE("Graph compiler rejects reading the surface before it is written") {
    Builder b;
    uint32_t surface = b.image("surface", true);
    b.node("early read", NodeKind::Transfer, {{surface, Access::TransferSrc}});
    b.node("draw", NodeKind::Graphics, {{surface, Access::ColorAttachment}});
    CHECK_THROWS_WITH_AS(b.compile(), doctest::Contains("reads the surface before it is written"), GraphError);
}

TEST_CASE("A disabled surface writer means no acquire or present") {
    Builder b;
    uint32_t surface = b.image("surface", true);
    b.node("draw", NodeKind::Graphics, {{surface, Access::ColorAttachment}});
    b.node("other", NodeKind::Compute);
    b.desc.nodes[0].enabled = false;
    auto compiled = b.compile();
    CHECK_FALSE(compiled.usesSurface);
    for (const auto& submission : compiled.submissions) {
        CHECK_FALSE(submission.acquiresSurface);
        CHECK_FALSE(submission.presentsSurface);
    }
}

TEST_CASE("Graph compiler puts everything on the only queue when there is one") {
    Builder b(1);
    uint32_t image = b.image("image");
    b.node("a", NodeKind::Compute, {{image, Access::StorageWrite}});
    b.node("b", NodeKind::Compute);
    b.node("c", NodeKind::Graphics, {{image, Access::SampledRead}});
    auto compiled = b.compile();
    REQUIRE(compiled.submissions.size() == 1);
    CHECK(compiled.submissions[0].nodes == std::vector<uint32_t>{0, 1, 2});
}

TEST_CASE("Graph compiler spreads independent compute work across queues") {
    Builder b(4);
    uint32_t x = b.image("x"), y = b.image("y"), z = b.image("z");
    uint32_t a = b.node("a", NodeKind::Compute, {{x, Access::StorageWrite}});
    uint32_t c = b.node("b", NodeKind::Compute, {{y, Access::StorageWrite}});
    uint32_t d = b.node("c", NodeKind::Compute, {{z, Access::StorageWrite}});
    uint32_t draw = b.node("draw", NodeKind::Graphics,
                           {{x, Access::SampledRead}, {y, Access::SampledRead}, {z, Access::SampledRead}});
    auto compiled = b.compile();
    // Each independent producer gets a queue of its own (not the primary); graphics stays primary
    std::set<int32_t> queues{compiled.nodeQueue[a], compiled.nodeQueue[c], compiled.nodeQueue[d]};
    CHECK(queues == std::set<int32_t>{1, 2, 3});
    CHECK(compiled.nodeQueue[draw] == 0);
    CHECK(compiled.submissions.size() == 4);
}

TEST_CASE("Graph compiler keeps dependent chains on one queue") {
    Builder b(4);
    uint32_t x = b.image("x"), y = b.image("y");
    uint32_t first = b.node("first", NodeKind::Compute, {{x, Access::StorageWrite}});
    uint32_t second = b.node("second", NodeKind::Compute, {{x, Access::StorageRead}, {y, Access::StorageWrite}});
    uint32_t third = b.node("third", NodeKind::Compute, {{y, Access::StorageRead}});
    auto compiled = b.compile();
    CHECK(compiled.nodeQueue[first] == compiled.nodeQueue[second]);
    CHECK(compiled.nodeQueue[second] == compiled.nodeQueue[third]);
    CHECK(compiled.submissions.size() == 1);
}

TEST_CASE("Graph compiler continues on the queue of a graphics ancestor when nothing runs in parallel") {
    Builder b(2);
    uint32_t color = b.image("color");
    uint32_t draw = b.node("draw", NodeKind::Graphics, {{color, Access::ColorAttachment}});
    uint32_t post = b.node("post", NodeKind::Compute, {{color, Access::StorageReadWrite}});
    auto compiled = b.compile();
    CHECK(compiled.nodeQueue[draw] == 0);
    CHECK(compiled.nodeQueue[post] == 0);
}

TEST_CASE("Graph compiler pins nodes to the primary queue on request") {
    Builder b(4);
    uint32_t node = b.node("pinned", NodeKind::Compute);
    b.desc.nodes[node].pinToPrimaryQueue = true;
    CHECK(b.compile().nodeQueue[node] == 0);
}

TEST_CASE("Graph compiler never makes a submission wait on a later one") {
    // a (primary) and b (async) are independent; c on the primary queue depends on both. c can't
    // join a's submission, which was created before b's: it would wait on a later submission.
    Builder b(2);
    uint32_t x = b.image("x"), y = b.image("y");
    uint32_t a = b.node("a", NodeKind::Graphics, {{x, Access::ColorAttachment}});
    uint32_t async = b.node("b", NodeKind::Compute, {{y, Access::StorageWrite}});
    uint32_t c = b.node("c", NodeKind::Graphics, {{x, Access::SampledRead}, {y, Access::SampledRead}});
    auto compiled = b.compile();
    CHECK(compiled.nodeQueue[a] == 0);
    CHECK(compiled.nodeQueue[async] == 1);
    CHECK(compiled.nodeQueue[c] == 0);
    CHECK(compiled.nodeSubmission[c] != compiled.nodeSubmission[a]);
    for (uint32_t node : {a, async, c}) {
        for (uint32_t predecessor : compiled.predecessors[node]) {
            CHECK(compiled.submissions[compiled.nodeSubmission[predecessor]].step
                  <= compiled.submissions[compiled.nodeSubmission[node]].step);
        }
    }
}

TEST_CASE("Graph compiler keeps a compute node after graphics on the primary queue when it depends on it") {
    Builder b(2);
    uint32_t x = b.image("x"), y = b.image("y");
    b.node("a", NodeKind::Graphics, {{x, Access::ColorAttachment}});
    uint32_t dependent = b.node("b", NodeKind::Compute, {{x, Access::SampledRead}, {y, Access::StorageWrite}});
    b.node("c", NodeKind::Graphics, {{y, Access::SampledRead}});
    auto compiled = b.compile();
    CHECK(compiled.nodeQueue[dependent] == 0);
    CHECK(compiled.submissions.size() == 1);
}

TEST_CASE("Graph compiler orders CPU nodes and splits submissions around them") {
    Builder b(1);
    uint32_t ubo = b.buffer("ubo", true);
    uint32_t result = b.buffer("result");
    uint32_t update = b.node("update", NodeKind::Cpu, {{ubo, Access::HostWrite}});
    uint32_t compute = b.node("compute", NodeKind::Compute, {{ubo, Access::UniformRead}, {result, Access::StorageBufferWrite}});
    uint32_t readback = b.node("readback", NodeKind::Cpu, {{result, Access::HostRead}});
    uint32_t after = b.node("after", NodeKind::Compute, {{result, Access::StorageBufferWrite}});
    auto compiled = b.compile();
    REQUIRE(compiled.steps.size() == 4);
    CHECK(compiled.steps[0].cpu);
    CHECK(compiled.steps[0].index == update);
    CHECK_FALSE(compiled.steps[1].cpu);
    CHECK(compiled.submissions[compiled.steps[1].index].nodes == std::vector<uint32_t>{compute});
    CHECK(compiled.steps[2].cpu);
    CHECK(compiled.steps[2].index == readback);
    // 'after' writes what the CPU node read (write after read), so it needs a new submission
    CHECK(compiled.submissions[compiled.steps[3].index].nodes == std::vector<uint32_t>{after});
    CHECK(compiled.warnings.empty());   // The uniform buffer is per frame
}

TEST_CASE("Graph compiler warns about host writes to shared resources the GPU reads") {
    Builder b(1);
    uint32_t ubo = b.buffer("ubo", false);
    b.node("update", NodeKind::Cpu, {{ubo, Access::HostWrite}});
    b.node("compute", NodeKind::Compute, {{ubo, Access::UniformRead}});
    auto compiled = b.compile();
    REQUIRE(compiled.warnings.size() == 1);
    CHECK(compiled.warnings[0].find("'ubo'") != std::string::npos);
}

TEST_CASE("Graph compiler places final transitions in the last submission using a resource") {
    Builder b(2);
    uint32_t texture = b.image("texture", false, true);
    uint32_t x = b.image("x");
    uint32_t a = b.node("a", NodeKind::Compute, {{texture, Access::SampledRead}, {x, Access::StorageWrite}});
    uint32_t draw = b.node("draw", NodeKind::Graphics, {{texture, Access::SampledRead}, {x, Access::SampledRead}});
    auto compiled = b.compile();
    const auto& last = compiled.submissions[compiled.nodeSubmission[draw]];
    CHECK(last.finalTransitions == std::vector<uint32_t>{texture});
    CHECK(compiled.submissions[compiled.nodeSubmission[a]].finalTransitions.empty());
}
