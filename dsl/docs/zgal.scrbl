#lang scribble/manual
@(require racket/string
          scribble/core
          "helpers.rkt")

@title{ZGAL: shader interfaces for the Vulkan wrapper}

ZGAL (Zach's Graphics Application Language) is a small language for the part of a shader that C++ has to agree with: the structs in
buffers, a material's bindings and push constants, vertex formats, and what one stage passes to the
next. A @tt{.zgal} file declares them once, and its compiler generates both sides: a C++ header
for the wrapper (@tt{include/}) and complete GLSL shaders, each the shader's own code (its
@deftech{body}) preceded by its material's interface. The @tt{.zgal} file is the source of truth,
so the shaders and the C++ can't disagree about a binding's order, a field's offset or a vertex
attribute's location.

ZGAL is written in Racket (@tt{dsl/}). This is its first stage: it describes interfaces, and shader
bodies are still GLSL. Later stages are planned to describe render graphs, shader bodies and
application state (see @secref["roadmap"]).

This document is built from the compiler: every example is compiled while it builds, and the
generated code it shows is the compiler's actual output. Build it with @tt{make docs}, which writes
@tt{build/docs/zgal.html} and a plain text copy, @tt{build/docs/zgal.txt}.

@table-of-contents[]

@; -----------------------------------------------------------------------------------------------
@section[#:tag "tour"]{A first material}

The scene example's tonemap pass reads the HDR image through a sampler, multiplies it by an
exposure and maps it to the display. Its interface, in @tt{examples/scene/scene.zgal}:

@(define tonemap-source #<<ZGAL
(graphics-material Tonemap
  (shaders "shaders/tonemap.vert" "shaders/tonemap.frag")
  (bindings
   (hdr texture #:stages (fragment))
   (hdrSampler sampler))
  (push
   (exposure float))
  (varyings
   (uv vec2))
  (outputs
   (outColor vec4)))
ZGAL
)
@(define tonemap-bodies
   (hash "shaders/tonemap.vert" (repository-file "examples/scene/shaders/tonemap.vert")
         "shaders/tonemap.frag" (repository-file "examples/scene/shaders/tonemap.frag")))

@(zgal-source tonemap-source)

The material has two shaders, two bindings (a texture used by the fragment shader, and a sampler),
a push constant, a value the vertex shader passes to the fragment shader, and one colour output.
The fragment shader's body uses them by name:

@(file-excerpt "examples/scene/shaders/tonemap.frag" #:from #rx"^void main")

Bindings and push constants are used through @tt{name()} accessors, and varyings and outputs as the
variables they are. The compiler writes everything before the body:

@(generated-file tonemap-source "tonemap.frag" #:bodies tonemap-bodies)

The @tt{#line} directives make glslc report errors at the line they came from: in the
@tt{.zgal} file for the interface, and in the body file for the body (see @secref["errors"]).

On the C++ side, the generated header declares a @tt{Tonemap} class deriving from
@tt{GraphicsMaterial}. It loads exactly the shaders it was generated with, takes its bindings by
name, and asks for its push constants through @tt{writePush}:

@(generated-file tonemap-source "example.zgal.h" #:bodies tonemap-bodies
                 #:from #rx"struct Bindings" #:to #rx"^    };")
@(generated-file tonemap-source "example.zgal.h" #:bodies tonemap-bodies
                 #:from #rx"Fills the push constants" #:to #rx"writePush")

The scene example subclasses it to read the exposure from its state:

@(file-excerpt "examples/scene/scene_example.cpp" #:from #rx"^class TonemapMaterial" #:to #rx"^};")

and creates it from the shaders @tt{loadShaders} finds in the context's shader library:

@(file-excerpt "examples/scene/scene_example.cpp" #:from #rx"Tonemap::loadShaders" #:to #rx"heap.sampler[(]linearSampler")

Nothing else describes the interface. Adding a binding means adding it to the @tt{.zgal} file,
which gives the shaders its accessor and the C++ a member of @tt{Bindings} to fill (creating the
material without an entry for it is a contract violation).

@; -----------------------------------------------------------------------------------------------
@section[#:tag "build"]{Building with ZGAL}

@subsection{What the compiler generates}

For @tt{name.zgal}, the compiler writes
@itemlist[
 @item{@tt{name.zgal.h}: the structs, vertex formats and a class per material
       (@secref["cpp"]).}
 @item{one complete shader per material shader, named like its body (e.g. @tt{tonemap.frag}), to
       compile with glslc (@secref["glsl"]).}
]
Shader bodies are paths relative to the @tt{.zgal} file. The shader library finds compiled shaders
by file name (@tt{tonemap.frag.spv}), so each body belongs to one material, and file names are
unique within a file.

@subsection{The Makefile}

The Makefile's @tt{zgal-rule} generates a @tt{.zgal} file's outputs into a directory, with a stamp
file that depends on the @tt{.zgal} file, its shader bodies and the compiler. The scene example's
rules:

@(file-excerpt "Makefile" #:from #rx"^SCENE_GEN =" #:to #rx"^[$][(]SCENE_OUT[)]/scene_example[.]o")

The generated shaders are compiled with @tt{-I} for the bodies' directory, so a body's
@tt{#include "common.glsl"} finds files beside it, and C++ is compiled with @tt{-I} for the
generated header. Rebuilding shaders (@tt{make shaders}) regenerates them first, so a running
example reloads edited bodies as before.

@subsection{The command line}

@verbatim{
racket dsl/zgal.rkt [--cpp-out DIR] [--glsl-out DIR] [--depfile FILE [--dep-target TARGET]] FILE.zgal
racket dsl/zgal.rkt --check FILE.zgal
}
@itemlist[
 @item{@tt{@literal{--cpp-out} DIR} writes @tt{DIR/FILE.zgal.h}.}
 @item{@tt{@literal{--glsl-out} DIR} writes each shader to @tt{DIR/<shader name>}.}
 @item{@tt{@literal{--depfile} FILE} writes a Makefile rule making @tt{TARGET} (by default, the header)
       depend on the @tt{.zgal} file and its shader bodies, with an empty rule per body (like
       @tt{gcc -MP}), so deleting a body doesn't break the build.}
 @item{@tt{@literal{--check}} only checks the file. Shader bodies aren't read, so a missing body isn't
       reported.}
]
Files whose contents wouldn't change aren't rewritten, so their timestamps don't trigger rebuilds.
Errors are printed as @tt{file:line:column: error: message [id]} and the exit status is 1.

@; -----------------------------------------------------------------------------------------------
@section[#:tag "language"]{The language}

@subsection{Syntax and names}

A @tt{.zgal} file is a sequence of s-expressions: @racket[struct], @racket[vertex-format],
@racket[graphics-material] and @racket[compute-material] forms, in any order except that a struct
or vertex format is declared before it's used. Comments start with @tt{;} and run to the end of the
line. The reader only reads data: @tt{#lang} and @tt{#reader}, which would run code, are rejected.

@racketgrammar*[
 #:literals (struct vertex-format graphics-material compute-material array)
 [file (code:line form ...)]
 [form (struct Name field ...+)
       (vertex-format Name field ...+)
       (graphics-material Name graphics-clause ...)
       (compute-material Name compute-clause ...)]
 [field (name type)]
 [type float int uint bool vec2 vec3 vec4 ivec2 ivec3 ivec4 uvec2 uvec3 uvec4 mat2 mat3 mat4
       StructName
       (array type length)]]

Every name becomes a GLSL and a C++ identifier, unchanged. Names are letters, digits and
underscores, start with a letter and don't contain @tt{__}. Write them as the shaders and C++ will
use them (@tt{shadowMap}, not @tt{shadow-map}). A name can't be
@itemlist[
 @item{a GLSL or C++ keyword or type (e.g. @tt{sampler}, @tt{output}, @tt{class});}
 @item{a GLSL built-in function: binding accessors are function-like macros, so a binding named
       @tt{texture} would replace GLSL's @tt{texture()};}
 @item{a name the wrapper's C++ declares (e.g. @tt{Material}, @tt{Handle});}
 @item{a name the generated code uses: @tt{main}, @tt{materialPush}, @tt{Bindings}, @tt{Push},
       @tt{Shaders}, @tt{loadShaders}, @tt{vertexLayout}, @tt{writePush}, or anything starting with
       @tt{gl_} or @tt{zgal}.}
]

@subsection[#:tag "types"]{Types and layout}

@tabular[
 #:style 'boxed
 #:sep (hspace 2)
 #:row-properties '(bottom-border ())
 (list
  (list @bold{Type} @bold{C++} @bold{Buffers and push constants} @bold{Vertex attributes, varyings, outputs})
  (list @elem{@tt{float}, @tt{int}, @tt{uint}} @elem{@tt{float}, @tt{int32_t}, @tt{uint32_t}} "yes" "yes")
  (list @tt{bool} "" "no: use uint" "no")
  (list @elem{@tt{vec2}-@tt{vec4}, @tt{ivec}, @tt{uvec}} @elem{@tt{glm::vec2}, ...} "yes" "yes")
  (list @elem{@tt{mat2}, @tt{mat4}} @elem{@tt{glm::mat2}, @tt{glm::mat4}} "yes" "no")
  (list @tt{mat3} "" "no: use mat4" "no")
  (list @tt{(array type length)} @tt{type name[length]} "yes, except arrays of 3-component vectors or arrays" "no")
  (list "structs" "the generated struct" "yes" "no"))]

Buffers and push constants have the std430 layout, which the generated C++ matches byte for byte.
Each member is aligned to its type's alignment: 4 bytes for scalars, 8 for 2-component vectors, 16
for 3- and 4-component vectors, a matrix's column's for matrices, and the largest member's for
structs, whose size is rounded up to it. Arrays' elements are each aligned to their type's
alignment, without std140's rounding to 16 bytes. So a scalar packs into the 4 bytes after a
@tt{vec3}, and a @tt{vec3} followed by a @tt{vec3} leaves a gap:

@(define layout-source #<<ZGAL
(struct Inner
  (a vec2)
  (b float))
(struct Example
  (position vec3)
  (radius float)
  (normal vec3)
  (offset vec2)
  (inner Inner)
  (weights (array float 3))
  (transform mat4))
ZGAL
)
@(zgal-source layout-source)
@(layout-table layout-source "Example")

GLM's types are 4-byte aligned, so the C++ compiler adds no padding of its own: the generated
struct places every member at its std430 offset with explicit padding members
(@tt{zgalPadding0_}, ...), then checks the layout with @tt{static_assert}s. Those fail if the
program defines @tt{GLM_FORCE_DEFAULT_ALIGNED_GENTYPES}, which changes GLM's alignment.
@tt{mat3} and arrays of 3-component vectors are rejected because std430 pads them where GLM's
types aren't padded.

@(generated-file layout-source "example.zgal.h" #:from #rx"^struct Example" #:to #rx"^};")

The padding members are default-initialized, so initialize the structs with designated
initializers (@tt{SceneUniforms{.viewProjection = ..., .cameraPosition = ...}}).

@subsection{@racket[struct]}

@racketgrammar[#:literals (struct) struct-form (struct Name (field type) ...+)]

Declares a struct for buffers and push constants, with the std430 layout. Every shader generated
from the file declares every struct, so bodies can use them whether or not a binding refers to
them; the header declares it as a C++ struct with that layout.

@subsection{@racket[vertex-format]}

@racketgrammar[#:literals (vertex-format) vertex-format-form (vertex-format Name (attribute type) ...+)]

Declares the vertices of a vertex buffer: a C++ struct, laid out as C++ lays it out, which materials
read through @racket[vertex-input]. Attributes are scalars or vectors of @tt{float}, @tt{int} or
@tt{uint}, read as 32-bit formats (e.g. @tt{VK_FORMAT_R32G32B32_SFLOAT} for a @tt{vec3}). An
attribute's location is its position in the format, whichever attributes a material reads.

@subsection{@racket[graphics-material]}

@racketgrammar*[
 #:literals (graphics-material shaders vertex-input bindings push varyings outputs flat noperspective smooth)
 [graphics-material-form (graphics-material Name graphics-clause ...)]
 [graphics-clause (shaders vertex-path fragment-path)
                  (vertex-input Format attribute ...)
                  (bindings binding ...)
                  (push (name type) ...)
                  (varyings varying ...)
                  (outputs (name type) ...)]
 [varying (name type) (name type flat) (name type noperspective) (name type smooth)]]

A material drawing with a vertex and a fragment shader. Each clause appears at most once;
@racket[shaders] and @racket[outputs] are required.
@itemlist[
 @item{@racket[shaders]: the vertex (@tt{.vert}) and fragment (@tt{.frag}) shader bodies, relative
       to the @tt{.zgal} file.}
 @item{@racket[vertex-input]: the vertex buffers' format and the attributes the vertex shader reads
       (all of them if none are named). The vertex shader gets each attribute as @tt{in} followed
       by its name (@tt{position} is @tt{inPosition}), at its location in the format. Without
       @racket[vertex-input] the material reads no vertex buffers, e.g. a fullscreen triangle built
       from @tt{gl_VertexIndex}.}
 @item{@racket[bindings]: the descriptor entries the shaders use (@secref["bindings"]).}
 @item{@racket[push]: push constants after the binding indices. The shaders read them through
       @tt{name()}; C++ writes them in @tt{writePush}. With the binding indices, they fit in
       @(number->string push-limit) bytes.}
 @item{@racket[varyings]: values the vertex shader writes (@tt{out}) and the fragment shader reads
       (@tt{in}), at locations in declaration order. Integer varyings must be @racket[flat];
       @racket[noperspective] interpolates linearly in screen space.}
 @item{@racket[outputs]: the fragment shader's colour outputs, one per colour attachment of the
       targets it draws into. Write @racket[(outputs)] for depth-only materials: the number is
       checked against the target's formats when the material is created.}
]

The scene example's lit material uses most of them:

@(file-excerpt "examples/scene/scene.zgal" #:from #rx"^[(]graphics-material Lit" #:to #rx"[(]outColor vec4")

@subsection{@racket[compute-material]}

@racketgrammar*[
 #:literals (compute-material shaders workgroup bindings push)
 [compute-material-form (compute-material Name compute-clause ...)]
 [compute-clause (shaders compute-path)
                 (workgroup x)
                 (workgroup x y)
                 (workgroup x y z)
                 (bindings binding ...)
                 (push (name type) ...)]]

A material dispatching a compute shader. @racket[shaders] and @racket[workgroup] are required.
@racket[workgroup] is the shader's local size (omitted dimensions are 1): the generated shader
declares it, and the C++ class exposes it, with @tt{workgroupsFor(x, y, z)} giving the
workgroups covering that many invocations for @tt{getDispatchDimensions}. Bindings and push
constants are as for graphics materials, without stages.

@(define compute-source #<<ZGAL
(struct Particle
  (position vec2)
  (velocity vec2))

(compute-material Simulate
  (shaders "simulate.comp")
  (workgroup 256)
  (bindings
   (particles (buffer-array Particle read-write)))
  (push
   (count uint)
   (dt float)))
ZGAL
)
@(define compute-bodies
   (hash "simulate.comp"
         "void main() {\n    uint i = gl_GlobalInvocationID.x;\n    if (i >= count()) {\n        return;\n    }\n    particles()[i].position += particles()[i].velocity * dt();\n}\n"))
@(zgal-example compute-source #:bodies compute-bodies #:show '("simulate.comp"))
@(generated-file compute-source "example.zgal.h" #:bodies compute-bodies
                 #:from #rx"The compute shader's workgroup size" #:to #rx"ComputeMaterial[(]context")

@subsection[#:tag "bindings"]{Bindings}

@racketgrammar*[
 #:literals (texture sampler storage-image buffer buffer-array read write read-write vertex fragment)
 [binding (name kind) (name kind #:stages (stage ...+))]
 [kind texture
       sampler
       (storage-image access)
       (buffer type access)
       (buffer-array type access)]
 [access read write read-write]
 [stage vertex fragment]]

A binding is a descriptor entry of the universal descriptor set (@tt{DescriptorHeap}). The
material pushes the entries' indices, in the bindings' order, and the shaders read them through an
accessor named after the binding:

@tabular[
 #:style 'boxed
 #:sep (hspace 2)
 #:row-properties '(bottom-border ())
 (list
  (list @bold{Kind} @bold{Entry} @bold{The accessor is} @bold{Render graph access})
  (list @tt{texture} @tt{heap.sampledImage} @tt{texture2D} @tt{SampledRead})
  (list @tt{sampler} @tt{heap.sampler} @tt{sampler} "none")
  (list @tt{(storage-image access)} @tt{heap.storageImage} @tt{image2D} @elem{@tt{StorageRead}, @tt{StorageWrite}, @tt{StorageReadWrite}})
  (list @tt{(buffer type access)} @tt{heap.storageBuffer} "the buffer's value, a type" @elem{@tt{StorageBufferRead}, ...})
  (list @tt{(buffer-array type access)} @tt{heap.storageBuffer} "an unsized array of type" @elem{@tt{StorageBufferRead}, ...}))]

Combine a texture and a sampler in GLSL: @tt{texture(sampler2D(albedo(), albedoSampler()), uv)}.
Buffer accessors are l-values, so shaders write through them: @tt{particles()[i].velocity = v}.
There are no uniform buffers: per-frame data goes in a @tt{buffer} binding with a buffer per frame
in flight (the scene example's @tt{LitMaterial} shows how).

The access is required for storage images and buffers. It's what the render graph synchronizes on,
and synchronization validation can't see accesses through the bindless arrays, so a write declared
as a read is a silent race: declare it accurately. ZGAL declares buffers @tt{readonly} when none
of the material's buffer bindings is written, but it can't enforce one binding's access while
another is written (MoltenVK can't alias a @tt{readonly} block with a writable one), and it
doesn't see which binding a body writes through. The render graph still rejects materials whose
shaders write storage buffers or images when no binding declares such a write.

@racket[#:stages] lists the stages using a graphics material's binding, so the graph synchronizes
with just those; by default, both. Samplers aren't synchronized and compute bindings are always
used by the compute stage, so neither takes @racket[#:stages].

@; -----------------------------------------------------------------------------------------------
@section[#:tag "glsl"]{Shader bodies}

A generated shader is, in order,
@itemlist[#:style 'ordered
 @item{@tt{#version 450}, @tt{GL_GOOGLE_cpp_style_line_directive} (for @tt{#line} with file names)
       and @tt{#include "bindless.glsl"} (the universal descriptor set's arrays);}
 @item{every struct in the file;}
 @item{the push constant block, @tt{materialPush}: a @tt{uint} index per binding, then the push
       fields;}
 @item{one buffer block declaration per buffer type (shared by bindings of the same type), all
       @tt{readonly} unless a buffer binding is written;}
 @item{an accessor macro per binding and push field;}
 @item{the stage's inputs and outputs: vertex inputs and varyings for vertex shaders, varyings and
       outputs for fragment shaders, the local size for compute shaders;}
 @item{the body, from its first line.}
]
So a body starts with its own code: no @tt{#version}, push constant block, bindings, inputs or
outputs (a @tt{#version} line is an error). It can define functions, constants and macros, and
@tt{#include} files beside it.

Accessors are function-like macros (@tt{#define hdr() (bindlessTextures[materialPush.hdr])}), so
they only expand where the name is followed by @tt{(}: the push constant block's members, struct
members and local variables can share their names (@tt{SceneUniforms scene = scene();}).

@; -----------------------------------------------------------------------------------------------
@section[#:tag "cpp"]{The generated C++}

The header includes the wrapper's material headers and declares, in file order, the structs, the
vertex formats, then a class per material. Using the material's members needs nothing but the
header.

@subsection{A material's class}

A material @tt{Name} becomes @tt{class Name : public GraphicsMaterial} (or @tt{ComputeMaterial}),
with
@itemlist[
 @item{@tt{struct Bindings}: a @tt{FrameBinding<DescriptorEntry>} per binding, by name. Give each
       one an entry, either shared by every frame or one per frame: a missing entry is a contract
       violation when the material is constructed.}
 @item{@tt{struct Push} (with push constants): the push fields, laid out as in the shaders.}
 @item{@tt{class Shaders} and @tt{static Result<Shaders> loadShaders(VulkanContext&)}, which loads
       the material's shaders from the shader library. The constructor takes a @tt{Shaders}, so
       the material can only be built from the shaders generated with its interface.}
 @item{Graphics materials: @tt{kColorOutputs}, the number of outputs, and with a
       @racket[vertex-input], @tt{static VertexLayout vertexLayout()}.}
 @item{Compute materials: @tt{kWorkgroupSizeX}, @tt{Y} and @tt{Z}, and
       @tt{static glm::vec3 workgroupsFor(x, y = 1, z = 1)}.}
 @item{The constructor: @tt{(context, shaders, bindings, formats, options)} for graphics materials
       and @tt{(context, shaders, bindings)} for compute materials, without @tt{bindings} when
       there are none. A graphics material's @tt{options.vertexLayout} must be left empty: the
       material sets it from its @racket[vertex-input].}
 @item{With push constants, @tt{virtual void writePush(uint32_t frameIndex, Push& push) = 0}. It's
       called whenever the material is bound, and the push constants are recorded into the command
       buffer then, so it may read state the CPU changes between frames.}
]
A material without push constants is a concrete class (a compute material still needs
@tt{update} and @tt{getDispatchDimensions}). Subclass the others, and use them like any material:
@tt{ctx.resources().create<T>(...)}, @tt{NodeBuilder::uses}, and so on.

@subsection{Checks when the material is used}

The generated code checks what the compiler can't, as contract violations:
@itemlist[
 @item{a binding without an entry (when constructed);}
 @item{a graphics material whose formats have a different number of colour attachments than its
       outputs, or options with a vertex layout (when constructed);}
 @item{an entry of the wrong kind, e.g. a sampler given for a texture, which would index the wrong
       array (whenever the material is bound, for that frame's entries).}
]

@; -----------------------------------------------------------------------------------------------
@section[#:tag "errors"]{Errors}

@subsection{Where errors are reported}

Errors in a @tt{.zgal} file are reported at the form responsible, as
@tt{file:line:column: error: message [id]}, the format editors recognize. The ids are listed
below.

Errors from glslc and the C++ compiler in generated code are reported where the code came from:
the generated files have @tt{#line} directives naming the @tt{.zgal} line of each declaration
and the body's lines. So an error in a body is reported at the body's line, and a C++ error in a
generated declaration, e.g. two @tt{.zgal} files declaring the same struct and included together,
at the @tt{.zgal} lines declaring it. Lines that don't come from either (e.g. the bodies of the
generated C++ functions) are reported at their own lines in the generated file.

@subsection{Every error}

Each error below shows a program reporting it and the message, produced by the compiler as this
document was built.

@(error-catalog-section)

@; -----------------------------------------------------------------------------------------------
@section[#:tag "roadmap"]{Limitations and plans}

What ZGAL doesn't describe yet:
@itemlist[
 @item{vertex attributes in other formats than 32-bit ones (e.g. normalized bytes), several vertex
       buffers, or per-instance attributes;}
 @item{images other than 2D ones, and arrays of bindings;}
 @item{sharing declarations between @tt{.zgal} files (each file is compiled alone);}
 @item{a shader used by more than one material.}
]

ZGAL is planned to grow in stages: render graphs and their resources; shader bodies in an
expression language with GLSL and C++ backends, from which bindings' accesses are inferred; and
application state. The compiler's checked program (@tt{dsl/private/ast.rkt}) describes interfaces
without assuming a backend, so other backends (e.g. WebGL) only add emitters.

@; -----------------------------------------------------------------------------------------------
@section[#:tag "maintaining"]{Working on ZGAL}

@subsection{The compiler}

@tabular[
 #:style 'boxed
 #:sep (hspace 2)
 (list
  (list @tt{dsl/zgal.rkt} "the command line")
  (list @tt{dsl/compile.rkt} "compiling a file: reading, checking and generating; the tests and this document use it")
  (list @tt{dsl/private/parse.rkt} "reading and checking a file into the program")
  (list @tt{dsl/private/ast.rkt} "the checked program, independent of any backend")
  (list @tt{dsl/private/layout.rkt} "std430 layouts, and which types each place accepts")
  (list @tt{dsl/private/names.rkt} "valid and reserved names")
  (list @tt{dsl/private/errors.rkt} "the error catalog")
  (list @tt{dsl/private/writer.rkt} "writing generated code with #line directives")
  (list @tt{dsl/private/glsl.rkt} "the GLSL backend")
  (list @tt{dsl/private/cpp.rkt} "the C++ backend")
  (list @tt{dsl/docs/} "this document, and the helpers compiling its examples")
  (list @tt{dsl/tests/} "the compiler's tests (make test-dsl)"))]

@subsection{Keeping this document accurate}

This document is part of the language: change it in the same commit as the compiler.
@itemlist[
 @item{A new form or clause gets a grammar, a description and an example here. Examples are
       compiled while the document builds (@tt{zgal-example}, @tt{generated-file}), so show
       generated code through them rather than pasting it, and quote the repository's files with
       @tt{file-excerpt}, which fails the build if its markers stop matching.}
 @item{A new error gets an entry in @tt{dsl/private/errors.rkt}'s catalog: a title, an explanation
       and an example. The catalog is rendered above, and the tests check that every example
       reports its error and that the catalog lists exactly the errors the compiler raises.}
 @item{A change to generated code changes the golden files: regenerate them with
       @tt{ZGAL_UPDATE_GOLDEN=1 raco test dsl/tests/golden-test.rkt} and review the difference.}
 @item{@tt{make test-dsl} builds this document and runs the tests; @tt{make test} includes it.}
]

@subsection{Tests}

@itemlist[
 @item{@tt{dsl/tests/layout-test.rkt}: std430 layouts.}
 @item{@tt{dsl/tests/errors-test.rkt}: the error catalog, and the errors' messages and locations.}
 @item{@tt{dsl/tests/golden-test.rkt}: the output for @tt{golden/features.zgal}, which uses every
       feature.}
 @item{@tt{dsl/tests/toolchain-test.rkt}: the generated shaders and header through glslc and the C++
       compiler (with the build's flags, from @tt{make test-dsl}), @tt{#line} mapping, and the
       command line.}
 @item{@tt{tests/gpu/ZgalTest.cpp}: generated materials on the device, checking that the C++
       structs and push constants have the layout the shaders write and read.}
]
