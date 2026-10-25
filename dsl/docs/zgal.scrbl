#lang scribble/manual
@(require racket/string
          scribble/core
          "helpers.rkt")

@title{ZGAL: shaders, render graphs and apps for the Vulkan wrapper}

ZGAL (Zach's Graphics Application Language) is a small language for a Vulkan application's
shaders and render graphs. A @tt{.zgal} file declares a material's interface (the structs in its
buffers, its bindings and push constants, its vertex format, what one stage passes to the next)
once, and its shaders in an expression language. The compiler generates both sides: complete GLSL
shaders, and a C++ header for the wrapper (@tt{include/}) with a class per material. The
@tt{.zgal} file is the source of truth, so the shaders and the C++ can't disagree about a
binding's order, a field's offset or a vertex attribute's location, and bindings' accesses, which
the render graph synchronizes on, are inferred from what the shaders do.

A @tt{.zgal} file can also declare render graphs (@secref["graphs"]): their parameters, inputs,
resources, materials and nodes. The compiler generates a @tt{build()} function adding them to a
@tt{RenderGraph}, so C++ supplies only what changes at runtime: meshes, imported images and
per-frame values. Functions that don't use textures are also compiled to C++, so the CPU and the
shaders can share code.

And it can declare apps (@secref["apps"]): an application's state, the handlers computing its next
state from input events and each frame, and the graph it runs, whose per-frame inputs are computed
from the state. The compiler generates a class running them. A @racket[main] makes the file a
program (@secref["main"]): the compiler writes its @tt{main} too, with a command line, and C++ only
gives what ZGAL can't; otherwise C++'s @tt{main} creates the app and runs it in a window (or
headless).

ZGAL is written in Racket (@tt{dsl/}). Shaders can also still be written in GLSL, with only their
interface in ZGAL (@secref["glsl"]).

This document is built from the compiler: every example is compiled while it builds, and the
generated code it shows is the compiler's actual output. Build it with @tt{make docs}, which writes
@tt{build/docs/zgal.html} and a plain text copy, @tt{build/docs/zgal.txt}.

@table-of-contents[]

@; -----------------------------------------------------------------------------------------------
@section[#:tag "tour"]{A first material}

The scene example's tonemap pass reads the HDR image through a sampler, multiplies it by an
exposure and maps it to the display with an ACES curve, drawing a triangle that covers the screen.
In @tt{examples/scene/scene.zgal}:

@(define tonemap-source
   (string-append
    "(import color)\n\n"
    (repository-excerpt "examples/scene/scene.zgal" #:from #rx"^[(]graphics-material Tonemap" #:to #rx"exposure[)][)] 1[.]0[)][)][)]$")))

@(zgal-source tonemap-source)

The material has two bindings (a texture and a sampler), a push constant, a value the vertex shader
passes to the fragment shader (a varying), and one colour output. Its @racket[vertex] and
@racket[fragment] clauses are its shaders: expressions using those names. The vertex shader gives
its results (the position, and every varying), and the fragment shader's value is its output.
@tt{acesFilmic} comes from the standard library's @tt{color} module (@secref["library"]), whose
functions become the file's, for any shader to call.

The compiler generates a GLSL shader per stage, the interface followed by the body:

@(generated-file tonemap-source "Tonemap.frag")

The @tt{#line} directives make glslc report errors at the line of the @tt{.zgal} file they came
from (see @secref["errors"]). The C++ header declares a @tt{Tonemap} class deriving from
@tt{GraphicsMaterial}. It loads exactly the shaders generated for it, takes its bindings by name
(with the stages using them, inferred from the shaders), and asks for its push constants through
@tt{writePush}:

@(generated-file tonemap-source "example.zgal.h" #:from #rx"struct Bindings" #:to #rx"^    };")
@(generated-file tonemap-source "example.zgal.h" #:from #rx"Fills the push constants" #:to #rx"writePush")

@tt{acesFilmic} uses no textures, so it's compiled to C++ too, for the CPU to use:

@(generated-file tonemap-source "example.zgal.h" #:from #rx"^inline glm::vec3 acesFilmic" #:to #rx"^}")

C++ can subclass a material to supply its push constants, and create it from the shaders
@tt{loadShaders} finds in the context's shader library, as this GPU test does with another
material:

@(file-excerpt "tests/gpu/ZgalTest.cpp" #:from #rx"^class TintedMaterial" #:to #rx"^};")
@(file-excerpt "tests/gpu/ZgalTest.cpp" #:from #rx"auto shaders = Tinted::loadShaders" #:to #rx"glm::vec4[(]1.0f, 0.5f, 0.5f, 1.0f[)][)];")

The scene example doesn't write any of that: its graph, declared in the same file, instantiates
the material, binds the HDR image and a sampler to it, and computes the exposure each frame from a
per-frame input (@secref["graphs"]):

@(file-excerpt "examples/scene/scene.zgal" #:from #rx"[(]material tonemap Tonemap" #:to #rx"#:push [(][(]exposure exposure")

Nothing else describes the interface. Adding a binding means adding it to the @tt{.zgal} file,
which gives the shaders its name and the C++ a member of @tt{Bindings} to fill (creating the
material without an entry for it is a contract violation), or a graph a binding to give.

@; -----------------------------------------------------------------------------------------------
@section[#:tag "build"]{Building with ZGAL}

@subsection{What the compiler generates}

For @tt{name.zgal}, the compiler writes
@itemlist[
 @item{@tt{name.zgal.h}: the structs, vertex formats, a class per material (@secref["cpp"]), a
       struct per graph (@secref["graphs"]) and a class per app (@secref["apps"]), all in
       @tt{namespace name} (e.g. @tt{scene::SceneApp}), so files can declare the same names. The
       file's name must therefore be a valid name.}
 @item{@tt{name.zgal.main.cpp}, for a program (@secref["main"]): its @tt{main}.}
 @item{one complete shader per material shader, named after its material and stage (e.g.
       @tt{Tonemap.frag}), or for a GLSL body like its file, to compile with glslc
       (@secref["glsl"]).}
]
Compiled for the web, it writes a WGSL module per material shader instead (@secref["web"]).
GLSL bodies are paths relative to the @tt{.zgal} file. The shader library finds compiled shaders
by file name (@tt{Tonemap.frag.spv}), so each GLSL body belongs to one material, and shader names
are unique within a file.

@subsection[#:tag "building"]{The Makefile}

@tt{zgal.mk}, which the repository's Makefile includes, has the rules; other projects' Makefiles
include it too (e.g. from the wrapper as a submodule), defining @tt{CXX}, @tt{CFLAGS}, @tt{LDFLAGS}
and @tt{LDLIBS} first. It adds the platform's include and library paths (the Vulkan SDK's when
@tt{VULKAN_SDK} is set) to its rules' compilations and links. Its @tt{zgal-rule} generates a
@tt{.zgal} file's outputs into a directory, with a stamp file that depends on the @tt{.zgal} file,
its shader bodies and the compiler, and compiles its shaders. @tt{zgal-program} builds a program
(@secref["main"]) on it: the generated main, the program's own C++ (the @tt{.cpp} files beside the
@tt{.zgal} file), its SPIR-V in @tt{shaders/} and its assets (the files under @tt{assets/} beside
the @tt{.zgal} file) copied to @tt{assets/}, beside the executable, where the context finds them.
The scene example's rules:

@(file-excerpt "Makefile" #:from #rx"^SCENE_DIR =" #:to #rx"zgal-program,[$][(]SCENE_DIR[)]")

The generated shaders are compiled with @tt{-I} for the bodies' directory, so a body's
@tt{#include "common.glsl"} finds files beside it, and C++ is compiled with @tt{-I} for the
generated header. Rebuilding shaders (@tt{make shaders}) regenerates them first, so a running
example reloads edited bodies as before.

@tt{zgal-web-program} builds a program for the web (@secref["web"]) into a directory: its page,
its main built to JavaScript with the runtime, and its assets. The particles example's rule:

@(file-excerpt "Makefile" #:from #rx"^PARTICLES_WEB =" #:to #rx"zgal-web-program")

@subsection{The command line}

@verbatim{
racket dsl/zgal.rkt [--cpp-out DIR] [--glsl-out DIR] [--depfile FILE [--dep-target TARGET]] FILE.zgal
racket dsl/zgal.rkt --web-out DIR FILE.zgal
racket dsl/zgal.rkt --check FILE.zgal
}
@itemlist[
 @item{@tt{@literal{--cpp-out} DIR} writes @tt{DIR/FILE.zgal.h}, and for a program
       @tt{DIR/FILE.zgal.main.cpp}.}
 @item{@tt{@literal{--glsl-out} DIR} writes each shader to @tt{DIR/<shader name>}, and removes other
       shaders there (those of materials since renamed or removed), so DIR holds one file's shaders.}
 @item{@tt{@literal{--depfile} FILE} writes a Makefile rule making @tt{TARGET} (by default, the header)
       depend on the @tt{.zgal} file and its shader bodies, with an empty rule per body (like
       @tt{gcc -MP}), so deleting a body doesn't break the build.}
 @item{@tt{@literal{--web-out} DIR} writes the TypeScript module (@secref["web"]) to
       @tt{DIR/FILE.zgal.ts} (for a program its main, @tt{DIR/FILE.zgal.main.ts}, and its page,
       @tt{DIR/FILE.zgal.html}) and each shader's WGSL module to
       @tt{DIR/<shader name>.wgsl} (the
       TypeScript holds them too; they're written for tools and reading), and removes other WGSL
       modules there. Given alone, it doesn't generate the C++ and GLSL.}
 @item{@tt{@literal{--check}} only checks the file. Shader bodies aren't read, so a missing body isn't
       reported.}
]
Files whose contents wouldn't change aren't rewritten, so their timestamps don't trigger rebuilds.
Errors are printed as @tt{file:line:column: error: message [id]} and the exit status is 1.

@; -----------------------------------------------------------------------------------------------
@section[#:tag "language"]{The language}

@subsection{Syntax and names}

A @tt{.zgal} file is a sequence of s-expressions: @racket[import] (@secref["imports"]), @racket[struct], @racket[vertex-format],
@racket[define] (functions and constants, @secref["functions"]), @racket[graphics-material],
@racket[compute-material], @racket[graph] and @racket[app] forms, in any order except that anything is declared
before it's used. Comments start with @tt{;} and run to the end of the
line. The reader only reads data: @tt{#lang} and @tt{#reader}, which would run code, are rejected.

@racketgrammar*[
 #:literals (import struct vertex-format define graphics-material compute-material graph app array :)
 [file (code:line form ...)]
 [form (import module)
       (import "file.zgal")
       (extern-glsl "file.glsl" ((name [parameter : type] ...) : type) ...+)
       (struct Name field ...+)
       (vertex-format Name attribute ...+)
       (define (name [parameter : type] ...) : type body ...+)
       (define name expression)
       (graphics-material Name graphics-clause ...)
       (compute-material Name compute-clause ...)
       (graph Name graph-item ...)
       (app Name app-clause ...)]
 [field (name type)]
 [type float int uint bool uint8 int8 uint16 int16
       vec2 vec3 vec4 ivec2 ivec3 ivec4 uvec2 uvec3 uvec4 mat2 mat3 mat4
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

@subsection[#:tag "imports"]{Imports and the standard library}

@racket[(import module)] imports a module of the standard library, and
@racket[(import "file.zgal")] a file, relative to the importing one (or by an absolute path). An
imported file holds structs, vertex formats, functions, constants, materials, graphs, apps and
imports (not a @racket[main]: a program is its own file), which become declarations of the
importing file, as if written where it imports them: so they're in its namespace, compile to its
shaders and header, and their names can't be declared again. Programs share materials, graphs and
apps this way, each compiling its own copy, e.g. a program per platform running one app. A file imported twice (even through other
imports) is read once, and a file can't import itself.

@subsubsection[#:tag "library"]{The standard library}

Modules in @tt{dsl/lib}, each a @tt{.zgal} file:

@(library-modules)

@subsection[#:tag "types"]{Types and layout}

@tabular[
 #:style 'boxed
 #:sep (hspace 2)
 #:row-properties '(bottom-border ())
 (list
  (list @bold{Type} @bold{C++} @bold{Buffers and push constants} @bold{Vertex attributes, varyings, outputs})
  (list @elem{@tt{float}, @tt{int}, @tt{uint}} @elem{@tt{float}, @tt{int32_t}, @tt{uint32_t}} "yes" "yes")
  (list @tt{bool} "" "no: use uint" "no")
  (list @elem{@tt{uint8}, @tt{int8}, @tt{uint16}, @tt{int16}} @elem{@tt{uint8_t}, ...} "buffers, not push constants" "no")
  (list @elem{@tt{vec2}-@tt{vec4}, @tt{ivec}, @tt{uvec}} @elem{@tt{glm::vec2}, ...} "yes" "yes")
  (list @elem{@tt{mat2}, @tt{mat4}} @elem{@tt{glm::mat2}, @tt{glm::mat4}} "yes" "no")
  (list @tt{mat3} "" "no: use mat4" "no")
  (list @tt{(array type length)} @tt{type name[length]} "yes, except arrays of 3-component vectors or arrays" "no")
  (list "structs" "the generated struct" "yes" "no"))]

Buffers and push constants have the std430 layout, which the generated C++ matches byte for byte.
Each member is aligned to its type's alignment: 4 bytes for scalars (1 and 2 for 8- and 16-bit
integers), 8 for 2-component vectors, 16
for 3- and 4-component vectors, a matrix's column's for matrices, and the largest member's for
structs, whose size is rounded up to it. Arrays' elements are each aligned to their type's
alignment, without std140's rounding to 16 bytes. So a scalar packs into the 4 bytes after a
@tt{vec3}, and a @tt{vec3} followed by a @tt{vec3} leaves a gap:

@(define image-file-source #<<ZGAL
(graph Textures
  (input maskPath path)                       ; Chosen by C++, e.g. from the command line
  (image-file albedo "textures/albedo.png" #:color srgb)
  (image-file mask maskPath #:color linear))
ZGAL
   )

@(define buffer-file-source #<<ZGAL
(struct Sprite (y uint8) (tile uint8) (attributes uint8) (x uint8))

(compute-material Move
  (workgroup 64)
  (bindings (sprites (buffer-array Sprite)))
  (compute
   (let* ([s (at sprites globalInvocationId.x)])
     (write! (at sprites globalInvocationId.x) (with s #:x (uint8 (+ s.x 1)))))))

(graph Sprites
  (input level path)
  ;; Each file holds whole Sprites, as a memory dump would: the buffer holds all of them, in order
  (buffer-file sprites Sprite "sprites/player.bin" level)
  (material moving Move sprites #:invocations (64))
  (dispatch move moving))
ZGAL
   )

@(define model-source #<<ZGAL
(vertex-format Vertex
  (position vec3)
  (normal vec3 #:as snorm16)
  (uv vec2)
  (color vec4 #:as unorm8 #:default (vec4 1 1 1 1)))   ; White when a file has no colours

(graphics-material Paint
  (vertex-input Vertex)
  (bindings (baseColors (array texture)) (factors (buffer-array vec4)) (linear sampler))
  (varyings (texCoord vec2) (tint vec4) (material uint flat))
  (outputs (paint vec4))
  (vertex (results #:position (vec4 position 1.0) #:texCoord uv #:tint color
                   #:material (uint instanceIndex)))
  (fragment (* tint (at factors material) (sample (at baseColors material) linear texCoord))))

(graph Ship
  (model ship "ship.glb" Vertex #:base-colors shipColors #:base-color-factors shipFactors)
  (sampler linear)
  (image canvas rgba8 (fixed 256 256) #:frames shared)
  (target onCanvas (color canvas))
  (material paint Paint (baseColors shipColors) (factors shipFactors) linear)
  (draw-meshes draw onCanvas ship paint))
ZGAL
   )

@(define packed-source #<<ZGAL
(vertex-format Vertex
  (position vec3)
  (normal vec3 #:as snorm16)
  (uv vec2 #:as float16)
  (color vec4 #:as unorm8))
ZGAL
   )

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
types aren't padded. Padding is @tt{uint32_t}s, or bytes where 8- and 16-bit members leave gaps of
other sizes.

@tt{uint8}, @tt{int8}, @tt{uint16} and @tt{int16} are storage types, for packing buffers (and
shared memory): shader bodies read them as @tt{uint} or @tt{int} (@secref["small-integers"]).
Materials using them need device features (8- and 16-bit integers and storage) that the wrapper
doesn't enable, which their generated classes report (@secref["cpp"]). They can't be push
constants, which would need more features.

@(generated-file layout-source "example.zgal.h" #:from #rx"^struct Example" #:to #rx"^};")

The padding members are default-initialized, so initialize the structs with designated
initializers (@tt{SceneUniforms{.viewProjection = ..., .cameraPosition = ...}}).

@subsection{@racket[struct]}

@racketgrammar[#:literals (struct) struct-form (struct Name (field type) ...+)]

Declares a struct for buffers and push constants, with the std430 layout. Every shader generated
from the file declares every struct, so bodies can use them whether or not a binding refers to
them; the header declares it as a C++ struct with that layout. Array fields are @tt{std::array}s,
so C++ copies and assigns them like any other value.

@subsection{@racket[vertex-format]}

@racketgrammar*[#:literals (vertex-format)
 [vertex-format-form (vertex-format Name attribute ...+)]
 [attribute (name type attribute-option ...)]
 [attribute-option (code:line #:as storage) (code:line #:default value)]]

Declares the vertices of a vertex buffer: a C++ struct, laid out as C++ lays it out, which materials
read through @racket[vertex-input]. Attributes are scalars or vectors of @tt{float}, @tt{int} or
@tt{uint}, which is what shaders read. By default vertex buffers hold them in 32-bit formats
(e.g. @tt{VK_FORMAT_R32G32B32_SFLOAT} for a @tt{vec3}). An attribute's location is its position in
the format, whichever attributes a material reads.

@racket[#:as] stores an attribute in fewer bits, which the device converts as it reads them, so
shaders are unchanged:
@itemlist[
 @item{@tt{float} attributes: @tt{float16} (halves), @tt{unorm8} and @tt{unorm16} (unsigned
       integers read as [0, 1]), @tt{snorm8} and @tt{snorm16} (signed integers read as [-1, 1]);}
 @item{@tt{uint} attributes: @tt{uint8} and @tt{uint16}; @tt{int} attributes: @tt{int8} and
       @tt{int16}.}]

In C++ such an attribute is GLM's sized vector (@tt{glm::u8vec4}, @tt{glm::i16vec2}, ...) or a sized
integer, aligned to 4 bytes (as Metal requires; so the struct's size is a multiple of 4 too). GLM's
@tt{gtc/packing.hpp} converts floats to it, as the generated comment says. Not every device reads
every format from vertex buffers (3-component 8-bit formats are the usual gap): building a pipeline
reading one it can't fails with @tt{ErrorCode::Unsupported}.

@racket[#:default] gives the value a graph's @racket[model] gives the attribute (a number, or a
vector like @racket[(vec4 1 1 1 1)]) when its file doesn't (@secref["assets"]).

@(zgal-example packed-source #:show '())
@(generated-file packed-source "example.zgal.h" #:from #rx"^struct Vertex" #:to #rx"^};")

@subsection{@racket[graphics-material]}

@racketgrammar*[
 #:literals (graphics-material shaders vertex fragment vertex-input bindings push varyings outputs define : flat noperspective smooth)
 [graphics-material-form (graphics-material Name graphics-clause ...)]
 [graphics-clause (vertex body ...)
                  (fragment body ...)
                  (shaders glsl-path ...+)
                  (vertex-input Format attribute ...)
                  (bindings binding ...)
                  (push (name type) ...)
                  (varyings varying ...)
                  (outputs (name type) ...)
                  (define (name [parameter : type] ...) : result-type body ...+)]
 [varying (name type) (name type flat) (name type noperspective) (name type smooth)]]

A material drawing with a vertex and a fragment shader. Each clause but @racket[define] appears at
most once, and @racket[outputs] is required.
@itemlist[
 @item{@racket[vertex], @racket[fragment]: the shaders, written in ZGAL (@secref["bodies"]).}
 @item{@racket[shaders]: shaders written in GLSL instead (@secref["glsl"]): paths of @tt{.vert}
       and @tt{.frag} bodies, relative to the @tt{.zgal} file. A material can have one stage in
       each.}
 @item{@racket[vertex-input]: the vertex buffers' format and the attributes the vertex shader reads
       (all of them if none are named), at their locations in the format. The vertex shader reads
       an attribute by its name (in GLSL bodies, @tt{in} followed by its name: @tt{inPosition}).
       Without @racket[vertex-input] the material reads no vertex buffers, e.g. a fullscreen
       triangle built from the vertex index.}
 @item{@racket[bindings]: the descriptor entries the shaders use (@secref["bindings"]).}
 @item{@racket[push]: push constants after the binding indices. The shaders read them by name;
       C++ writes them in @tt{writePush}, or a graph computes them. With the binding indices, they
       fit in @(number->string push-limit) bytes.}
 @item{@racket[varyings]: values the vertex shader writes (@tt{out}) and the fragment shader reads
       (@tt{in}), at locations in declaration order. Integer varyings must be @racket[flat];
       @racket[noperspective] interpolates linearly in screen space.}
 @item{@racket[outputs]: the fragment shader's colour outputs, one per colour attachment of the
       targets it draws into. Write @racket[(outputs)] for depth-only materials: the number is
       checked against the target's formats when the material is created.}
 @item{@racket[define]: functions of the material, which its shaders call and which read its
       bindings and push constants (@secref["material-functions"]).}
]

The scene example's lit material uses most of them:

@(file-excerpt "examples/scene/scene.zgal" #:from #rx"^[(]graphics-material Lit" #:to #rx"[(]vec4 [(][+] direct ambient[)] 1[.]0[)][)][)]")

@subsection{@racket[compute-material]}

@racketgrammar*[
 #:literals (compute-material compute shaders workgroup bindings push shared define :)
 [compute-material-form (compute-material Name compute-clause ...)]
 [compute-clause (compute body ...)
                 (shaders glsl-path)
                 (workgroup x)
                 (workgroup x y)
                 (workgroup x y z)
                 (bindings binding ...)
                 (push (name type) ...)
                 (shared (name type) ...)
                 (define (name [parameter : type] ...) : result-type body ...+)]]

A material dispatching a compute shader: a @racket[compute] clause (in ZGAL) or a @tt{.comp} in
@racket[shaders] (in GLSL). @racket[workgroup] is required.
@racket[workgroup] is the shader's local size (omitted dimensions are 1): the generated shader
declares it, and the C++ class exposes it, with @tt{workgroupsFor(x, y, z)} giving the
workgroups covering that many invocations for @tt{getDispatchDimensions}. Bindings and push
constants are as for graphics materials, without stages, and so are functions (@racket[define]). @racket[shared] declares workgroup shared
variables, which a workgroup's invocations share (@secref["shared"]); GLSL bodies use them by name
too.

@(define compute-source #<<ZGAL
(struct Particle
  (position vec2)
  (velocity vec2))

(compute-material Simulate
  (workgroup 256)
  (bindings
   (particles (buffer-array Particle)))
  (push
   (count uint)
   (dt float))
  (compute
   (let* ([i globalInvocationId.x]
          [p (at particles i)])
     (when (< i count)
       (write! (at particles i) (with p #:position (+ p.position (* p.velocity dt))))))))
ZGAL
)
@(zgal-example compute-source #:show '("Simulate.comp"))
@(generated-file compute-source "example.zgal.h"
                 #:from #rx"The compute shader's workgroup size" #:to #rx"ComputeMaterial[(]context")

@subsection[#:tag "bindings"]{Bindings}

@racketgrammar*[
 #:literals (texture sampler storage-image buffer buffer-array array read write read-write vertex fragment)
 [binding (name kind) (name kind #:stages (stage ...+))]
 [kind texture
       sampler
       (storage-image maybe-access)
       (buffer type maybe-access)
       (buffer-array type maybe-access)
       (array kind)]
 [maybe-access (code:line) access]
 [access read write read-write]
 [stage vertex fragment]]

A binding is a descriptor entry of the universal descriptor set (@tt{DescriptorHeap}). The
material pushes the entries' indices, in the bindings' order, and the shaders use the binding by
its name (in GLSL bodies, through an accessor, @tt{name()}):

@tabular[
 #:style 'boxed
 #:sep (hspace 2)
 #:row-properties '(bottom-border ())
 (list
  (list @bold{Kind} @bold{Entry} @bold{In a shader, the binding is} @bold{Render graph access})
  (list @tt{texture} @tt{heap.sampledImage} @tt{texture2D} @tt{SampledRead})
  (list @tt{sampler} @tt{heap.sampler} @tt{sampler} "none")
  (list @tt{(storage-image access)} @tt{heap.storageImage} @tt{image2D} @elem{@tt{StorageRead}, @tt{StorageWrite}, @tt{StorageReadWrite}})
  (list @tt{(buffer type access)} @tt{heap.storageBuffer} "the buffer's value, a type" @elem{@tt{StorageBufferRead}, ...})
  (list @tt{(buffer-array type access)} @tt{heap.storageBuffer} "an unsized array of type" @elem{@tt{StorageBufferRead}, ...}))]

There are no uniform buffers: per-frame data goes in a @tt{buffer} binding with a buffer per frame
in flight (a graph's @racket[uniform] makes them).

A storage image's or buffer's access is what the render graph synchronizes on, and
synchronization validation can't see accesses through the bindless arrays, so a write declared as
a read would be a silent race. When all of a material's shaders are ZGAL bodies, accesses are
inferred from what they do (@secref["inference"]), and a declared one must agree. When one is a
GLSL body, the access is required: declare it accurately. ZGAL declares buffers @tt{readonly}
when none of the material's buffer bindings is written (MoltenVK can't alias a @tt{readonly}
block with a writable one, so it's all or none), and the render graph rejects materials whose
shaders write storage buffers or images when no binding declares such a write.

@racket[#:stages] lists the stages using a graphics material's binding, so the graph synchronizes
with just those. With ZGAL bodies they're inferred; with GLSL ones they default to both. Samplers
aren't synchronized and compute bindings are always used by the compute stage, so neither takes
@racket[#:stages].

@subsubsection[#:tag "arrays"]{Arrays of bindings}

@racket[(array kind)] is a binding holding any number of entries of another kind (but samplers),
which shaders choose between: e.g. a set of textures, or the buffers of several meshes. Push
constants can't hold many indices, so C++ gives an array as a @tt{DescriptorArray}
(@tt{include/descriptors/DescriptorArray.h}), which keeps the entries' indices in a small buffer
per frame in flight (the count, then each index) and rewrites it when they change; the material
pushes that buffer's index. Bodies choose an element with @racket[(at binding i)], which different
invocations can choose differently (the index is @tt{nonuniformEXT}), and @racket[(array-length
binding)] is how many there are. The elements share the array's access, and a graph's node using
the material uses every element.

@(zgal-example
  (string-append
   "(compute-material Gather\n"
   "  (workgroup 64)\n"
   "  (bindings\n"
   "   (layers (array texture))\n"
   "   (counts (array (buffer-array uint)))\n"
   "   (widths (buffer-array uint)))\n"
   "  (compute\n"
   "   (let* ([i globalInvocationId.x])\n"
   "     (when (< i (array-length layers))\n"
   "       (let* ([layer (at layers i)])\n"
   "         (write! (at widths i) (+ (uint (.x (texture-size layer 0))) (at (at counts i) 0))))))))\n")
  #:show '("Gather.comp")
  #:from #rx"#define layers" #:to #rx"zgalLength_counts")

@; -----------------------------------------------------------------------------------------------
@section[#:tag "bodies"]{Shader bodies}

A material's @racket[vertex], @racket[fragment] and @racket[compute] clauses are its shaders,
written in ZGAL's expression language. Bodies are expressions: values are bound once with
@racket[let*] and never changed, loops are folds over ranges, and the only effects are writes to
buffers and images. The compiler checks them, infers what they do to bindings, and lowers them to
GLSL statements.

@subsection{Stages and results}

@itemlist[
 @item{A @racket[vertex] body gives its results: @racket[(results #:position p #:varying v ...)],
       its clip-space position (a @tt{vec4}) and every varying.}
 @item{A @racket[fragment] body gives @racket[(results #:output v ...)], every output, or, with one
       output, just its value. A material without outputs (depth only) has an empty
       @racket[(fragment)].}
 @item{A @racket[compute] body has no results: it writes buffers and images.}
]
A body's last expression (e.g. inside @racket[let*] and @racket[if]) gives its results, so
@racket[(let* ([c ...]) (results #:position ... ))] is the usual shape.

@subsection{Names}

A body names, besides its own locals:
@itemlist[
 @item{the material's bindings: a texture is a @tt{texture2D}, a sampler a @tt{sampler}, a storage
       image an @tt{image2D}, a @tt{buffer} binding its value, a @tt{buffer-array} binding an
       array of its element type, and an @racket[array] binding elements of its kind, chosen with
       @racket[at] (@secref["arrays"]);}
 @item{its push constants, by name;}
 @item{in a compute shader, its shared variables, by name;}
 @item{in a vertex shader, its @racket[vertex-input]'s attributes, by name;}
 @item{in a fragment shader, the varyings, by name;}
 @item{the file's constants and functions (@secref["functions"]);}
 @item{its stage's built-in variables:}
]
@(stage-variable-table)

A stage's outputs (a vertex shader's varyings, a fragment shader's outputs) can't be read, and
locals can't have their names, or the names of bindings, push constants, functions and constants.

Fields and swizzles follow names after dots: @tt{scene.lightDirection}, @tt{albedoRoughness.rgb},
@tt{p.position.x}. On other expressions, they're functions: @racket[(.rgb (sample hdr s uv))].

@subsection[#:tag "body-expressions"]{Expressions}

@racketgrammar*[
 #:literals (let* if cond else when unless begin fold sum for while range values with at array-length results)
 [expression literal
             name
             (.field expression)
             (let* ([name expression] ...) body ...+)
             (let* ([(name ...) expression] ...) body ...+)
             (if condition expression expression)
             (cond [condition expression] ... [else expression])
             (when condition effect ...+)
             (unless condition effect ...+)
             (begin effect ... expression)
             (fold ([accumulator initial] ...+) ([name range] ...+) expression)
             (sum ([name range] ...+) expression)
             (for ([name range] ...+) effect ...+)
             (while ([accumulator initial] ...+) condition expression)
             (values expression ...+)
             (with struct #:field value ...)
             (at array-or-vector index)
             (array-length array-binding)
             (results #:name expression ...)
             (operator expression ...)
             (constructor expression ...)
             (function expression ...)]
 [range (range from to) (range from to step)]]

@itemlist[
 @item{@bold{Literals}: numbers and @tt{#t}/@tt{#f}. Integer literals take the type their context
       needs, so @racket[(* 2 v)] is a @tt{vec3} when @tt{v} is, and a literal alone is an
       @tt{int}.}
 @item{@bold{Types} are GLSL's, and expressions are typed as in GLSL: arithmetic on scalars,
       vectors and matrices of the same kind (@racket[(* m v)] for a matrix and a vector), with
       no implicit conversions but literals'. Convert with @racket[(float x)], @racket[(int x)],
       @racket[(uint x)] and the vector constructors.}
 @item{@bold{Operators}: @racket[+], @racket[-] (also negation), @racket[*], @racket[/] (n-ary,
       left to right), @racket[remainder], @racket[bitand], @racket[bitor], @racket[bitxor],
       @racket[shl], @racket[shr] and @racket[bitnot] on ints and uints, @racket[<], @racket[<=],
       @racket[>], @racket[>=] on scalars, @racket[=] and @racket[not=], and @racket[and],
       @racket[or] and @racket[not].}
 @item{@bold{Constructors}: @racket[vec2] to @racket[vec4] (and @tt{ivec}, @tt{uvec}, @tt{bvec})
       from scalars and vectors with as many components (or one scalar for all of them),
       @racket[mat2] to @racket[mat4] from floats, columns, one float (the diagonal) or a matrix,
       structs from their fields in order, @racket[(array type element ...)], and
       @racket[(uint8 x)], @racket[(int8 x)], @racket[(uint16 x)] and @racket[(int16 x)], which
       narrow a value to write it (@secref["small-integers"]).}
 @item{@bold{Binding}: @racket[let*] binds in order; @racket[[(a b) e]] binds the values of a
       @racket[fold] with several accumulators (or of @racket[values]). A name bound to a texture,
       image or buffer array (e.g. @racket[[layer (at layers i)]]) stands for it: GLSL has no
       variables holding them.}
 @item{@bold{Loops}: @racket[fold] starts its accumulators at their initial values, and for each
       value of the ranges (nested, the first outermost; @racket[(range from to step)] goes from
       @tt{from} up to, and not including, @tt{to}, by @tt{step}, 1 by default) computes their new
       values from the body, a single value or @racket[(values ...)]. @racket[sum] adds its body
       up. @racket[for] runs effects, e.g. a grid-stride loop
       @racket[(for ([k (range i count (* 64 workgroupCount.x))]) ...)].
       @racket[while] is for loops whose end isn't a range: while its condition (which sees the
       accumulators) holds, the accumulators take the body's values, so a loop that breaks puts
       the reason in its condition. Its value is the accumulators' at the end.}
 @item{@racket[with] is a copy of a struct with some fields replaced:
       @racket[(with p #:velocity v)].}
]

@subsection{Effects}

@itemlist[
 @item{@racket[(write! place value)] writes a buffer binding or a shared variable: its value, or a
       field or element of it, e.g. @racket[(write! (at particles i) p)] or
       @racket[(write! result.count n)].}
 @item{@racket[(store! image texel value)] stores a @tt{vec4} to a storage image at an @tt{ivec2}
       texel.}
 @item{@racket[(barrier!)], in compute shaders, waits for the workgroup's invocations to reach it,
       after which they see each other's writes to shared variables (@secref["shared"]).
       @racket[(buffer-barrier!)] also makes their writes to buffers visible.}
 @item{Atomics (@secref["atomics"]) change a place and give its previous value.}
 @item{@racket[(return!)] ends a compute shader's invocation, e.g.
       @racket[(unless (< i count) (return!))]; @racket[(discard!)] a fragment shader's, writing
       nothing.}
]
Effects have no value (atomics have one, which a sequence may ignore). In a sequence (a body,
@racket[begin], @racket[when], @racket[let*]'s body) every expression but the last is an effect.

@subsection[#:tag "atomics"]{Atomics}

@racket[(atomic-add! place value)] adds to a place, atomically (every invocation's addition counts,
however many add at once), and gives the value it had before. A place is an element or field of a
buffer binding or shared variable, as for @racket[write!]. The others are @racket[atomic-min!],
@racket[atomic-max!], @racket[atomic-and!], @racket[atomic-or!], @racket[atomic-xor!],
@racket[atomic-exchange!] (which stores the value) and @racket[(atomic-compare-exchange! place
comparand value)] (which stores the value if the place holds the comparand). They work on
@tt{int}s and @tt{uint}s, and @racket[atomic-add!] and @racket[atomic-exchange!] on @tt{float}s
too, which needs devices' float atomics (the extension @tt{VK_EXT_shader_atomic_float}): the
material's @tt{requiredFeatures()} reports them. A binding an atomic uses is read and written.

@(define atomics-source #<<ZGAL
(compute-material Histogram
  (workgroup 64)
  (bindings
   (samples (buffer-array uint))
   (bins (buffer-array uint)))
  (push (count uint))
  (shared (local (array uint 16)))
  (compute
   (let* ([i globalInvocationId.x]
          [lane localInvocationId.x])
     (when (< lane 16) (write! (at local lane) 0))
     (barrier!)
     ;; Each workgroup counts into shared memory, then adds its counts to the buffer's
     (for ([k (range i count (* 64 workgroupCount.x))])
       (atomic-add! (at local (remainder (at samples k) 16)) 1))
     (barrier!)
     (when (< lane 16) (atomic-add! (at bins lane) (at local lane))))))
ZGAL
)
@(zgal-example atomics-source #:show '("Histogram.comp"))

An atomic's value goes into a temporary, so it happens exactly once, in order, whether or not the
body uses it.

@subsection[#:tag "subgroups"]{Subgroups and derivatives}

A subgroup is the invocations a device runs together (32 on Apple GPUs); the @tt{subgroup}
built-ins (@tt{subgroupAdd}, @tt{subgroupExclusiveAdd}, @tt{subgroupElect}, @tt{subgroupBallot},
@tt{subgroupShuffle}, ...) combine values across it without shared memory, and
@tt{subgroupSize}, @tt{subgroupInvocationId}, @tt{subgroupId} and @tt{subgroupCount} say where an
invocation is. Devices support them in some stages, and some kinds of operation: a material using
them has @tt{kSubgroupStages} and @tt{kSubgroupOperations}, which @tt{loadShaders} checks against
@tt{context.subgroupProperties()}.

Derivatives (@tt{dFdx}, @tt{dFdy}, @tt{fwidth}, and their @tt{Fine} and @tt{Coarse} variants)
measure how a value changes between neighbouring fragments, so only fragment shaders (and functions
they call) use them.

Functions using subgroups or derivatives have no C++ version.

@subsection[#:tag "small-integers"]{8- and 16-bit integers}

@tt{uint8}, @tt{int8}, @tt{uint16} and @tt{int16} pack buffers, structs and shared variables. Bodies
compute with 32-bit integers only: reading a small integer (a field, element, local or result)
gives it as a @tt{uint} (from @tt{uint8} and @tt{uint16}) or an @tt{int}, so arithmetic,
comparisons and built-ins are as usual. Writing one takes a small integer, so narrow explicitly,
@racket[(uint8 (+ b.count 1))], truncating as C++ does; an integer literal is converted for you,
if it fits.

@(define bytes-source #<<ZGAL
(struct Sprite
  (y uint8)
  (tile uint8)
  (attributes uint8)
  (x uint8))

(compute-material MoveSprites
  (workgroup 64)
  (bindings
   (sprites (buffer-array Sprite)))
  (compute
   (let* ([i globalInvocationId.x]
          [s (at sprites i)])
     (write! (at sprites i) (with s #:x (uint8 (+ s.x 1)) #:attributes 0)))))
ZGAL
)
@(zgal-example bytes-source #:show '("MoveSprites.comp"))

Shaders enable GLSL's @tt{GL_EXT_shader_explicit_arithmetic_types_int8} (and @tt{int16}) extensions
when the file uses small integers. The material needs the devices' @tt{shaderInt8} and
@tt{storageBuffer8BitAccess} (@tt{shaderInt16} and @tt{storageBuffer16BitAccess} for 16 bits):
its class's @tt{requiredFeatures()} lists them, for the application to add to the context's
features (@secref["cpp"]).

@subsection[#:tag "shared"]{Shared memory and barriers}

A compute material's @racket[shared] variables are shared by the invocations of a workgroup, for
cooperating on a result, e.g. a reduction. Invocations write them with @racket[write!] and wait
for each other's writes with @racket[(barrier!)] (GLSL's @tt{barrier()}, which orders shared
memory):

@(define reduce-source #<<ZGAL
(compute-material SumGroups
  (workgroup 64)
  (bindings
   (inputs (buffer-array float))
   (sums (buffer-array float)))
  (shared
   (partial (array float 64)))
  (compute
   (let* ([x localInvocationId.x])
     (write! (at partial x) (at inputs globalInvocationId.x))
     (barrier!)
     ;; Halve the active invocations, adding pairs, until one holds the sum
     (for ([level (range 0 6)])
       (let* ([stride (shr 32 (uint level))])
         (when (< x stride)
           (write! (at partial x) (+ (at partial x) (at partial (+ x stride)))))
         (barrier!)))
     (when (= x 0)
       (write! (at sums workgroupId.x) (at partial 0))))))
ZGAL
)
@(zgal-example reduce-source #:show '("SumGroups.comp"))

Every invocation of a workgroup must reach a barrier, or none, so a @racket[barrier!] can't be
under a condition, or in a loop, that could differ between invocations. Conditions and ranges
computed from literals, constants, push constants, @tt{workgroupId} and @tt{workgroupCount} (and
locals, loop variables and @racket[while] accumulators computed from them) are the same for the
whole workgroup, so a @racket[while] loop whose condition depends only on them runs as many times
in every invocation. So is an
element of a buffer the shader doesn't write at such an index (e.g. a count an earlier pass wrote:
nothing can change it during the dispatch); anything read from shared memory or a buffer the shader
writes, or computed from an invocation's id, isn't. Shared variables aren't
bindings, so they don't take part in access inference.

@subsection{Textures and images}

@tabular[
 #:style 'boxed
 #:sep (hspace 2)
 #:row-properties '(bottom-border ())
 (list
  (list @bold{Form} @bold{Value} @bold{GLSL})
  (list @tt{(sample t s uv)} @tt{vec4} @tt{texture(sampler2D(t, s), uv)})
  (list @tt{(sample-lod t s uv lod)} @tt{vec4} @tt{textureLod})
  (list @tt{(fetch t s texel level)} @tt{vec4} @elem{@tt{texelFetch}, at an @tt{ivec2} texel})
  (list @tt{(sample-compare t s uv reference)} @tt{float} "with a comparison sampler: the fraction of texels passing")
  (list @tt{(texture-size t level)} @tt{ivec2} @tt{textureSize})
  (list @tt{(image-load image texel)} @tt{vec4} @tt{imageLoad})
  (list @tt{(image-size image)} @tt{ivec2} @tt{imageSize})
  (list @tt{(store! image texel value)} "an effect" @tt{imageStore}))]

@subsection{Built-in functions}

GLSL's, with its overloads (scalars and vectors, and a scalar where GLSL takes one, e.g.
@racket[(clamp v 0.0 1.0)]):

@(builtin-function-list)

@racket[not] on a bool vector is GLSL's @tt{not}.

@subsection[#:tag "functions"]{Functions and constants}

@racketgrammar*[
 #:literals (define :)
 [function (define (name [parameter : type] ...) : result-type body ...+)]
 [constant (define name expression)]]

Functions and constants are declared at the top level and used by any shader (and function)
declared after them, so functions can't recurse. A function's parameters and result are any type,
plus @tt{texture2D}, @tt{sampler} and @tt{image2D} parameters and a @tt{void} result for functions
that only have effects; it can't name bindings or stage variables (pass them, or declare it in the
material: @secref["material-functions"]). Constants are
computed from literals, other constants, constructors, operators and built-in functions. A constant
computed only from integer literals (and such constants), like @racket[(define tiles 57)], is used as
a literal would be: it takes the type its context needs, so @racket[(< i tiles)] compares it with a
@tt{uint}. It's declared as an @tt{int} and converted where it's used as anything else
(@tt{uint(tiles)}); write @racket[(define tiles (uint 57))] to fix its type.

Shaders get the functions and constants they use. Those that use no textures, images, stage
variables or effects are also compiled to C++, as @tt{inline} functions and constants over GLM in
the generated header, so the CPU computes the same thing. The scene example's @tt{SunLight} uses
@tt{sunDirection} this way. C++ evaluates them with the CPU's floating point, so results agree to
the GPU's precision, and integer arithmetic exactly.

Metal (through MoltenVK) can't pass storage images to functions, so a function with an
@tt{image2D} parameter is inlined where it's called, its locals renamed so they can't clash with
the caller's.

@subsubsection[#:tag "material-functions"]{Materials' functions}

A material's @racket[(define (name [parameter : type] ...) : result-type body ...+)] clauses
declare functions of its own, which name its bindings and push constants as its shaders do, and
which its shaders (of every stage) and its later functions call. Like the file's functions they
can't name stage variables (pass them), and their names are the material's: no binding, push
constant or file function has them. Other materials don't see them. They're inlined where they're
called, so what a shader does to bindings through them is inferred like the rest of its body, and
they have no C++ version.

@(zgal-example
  (string-append
   "(compute-material Scale (workgroup 64)\n"
   "  (bindings (sources (buffer-array float)) (results (buffer-array float)))\n"
   "  (push (factor float) (n uint))\n"
   "  (define (scaled [i : uint]) : float (* (at sources i) factor))\n"
   "  (compute (let* ([i globalInvocationId.x])\n"
   "    (when (< i n) (write! (at results i) (+ (scaled i) (scaled 0)))))))\n")
  #:show '("Scale.comp")
  #:from #rx"^void main" #:to #rx"^}")

@subsubsection[#:tag "extern-glsl"]{Functions written in GLSL}

For what ZGAL bodies can't express yet, @racket[extern-glsl] declares functions written in a GLSL
file (relative to the declaring @tt{.zgal} file), which bodies call like any function:

@(define glsl-function-source #<<ZGAL
(extern-glsl "noise.glsl" ((noise [p : vec2]) : float))

(define (grain [p : vec2]) : float
  (* 0.5 (noise p)))

(compute-material Grain
  (workgroup 8 8)
  (bindings (target (storage-image)))
  (compute
   (store! target (ivec2 globalInvocationId.xy) (vec4 (grain (vec2 globalInvocationId.xy))))))
ZGAL
)
@(zgal-source glsl-function-source)
@(generated-file glsl-function-source "Grain.comp"
                 #:bodies (hash "noise.glsl" "float noise(vec2 p) {\n    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);\n}\n")
                 #:from #rx"^#line 1 \"noise[.]glsl\"" #:to #rx"^}")

A shader using one includes its file once (numbered as itself, so glslc reports errors at its
lines). Their parameters and results are values (no textures, images or effects), so they can't
touch bindings and access inference stays sound. They have no C++ version, so apps can't call them,
nor ZGAL functions calling them (@tt{grain} isn't in the header). Prefer ZGAL bodies: a GLSL
function is opaque to the compiler, and to backends other than GLSL.

@subsection[#:tag "inference"]{Inferred accesses}

When every shader of a material is a ZGAL body, the compiler infers each binding's use: a storage
image or buffer is read where the bodies read it (including through functions they pass it to)
and written where they @racket[write!] or @racket[store!] it, giving its access (@tt{read},
@tt{write} or @tt{read-write}). Querying an image's size (@tt{image-size}) touches no texels,
so an image a shader only sizes and stores to is written, not read and written (which WebGPU
devices without @tt{texture-formats-tier2} can't bind for most formats), and one it only sizes is
read. A graphics material's binding is used by the stages naming it.
A declared access or @racket[#:stages] must say the same. A storage binding no shader uses has no
access to infer, so it's an error unless declared. The accesses go into the generated C++ like
declared ones, so the render graph synchronizes exactly what the shaders do.

@subsection{What's generated}

The particles example's simulation, and the shader the compiler generates for it:

@(define particles-source
   (string-append
    (repository-excerpt "examples/particles/particles.zgal" #:from #rx"^[(]struct SimParams" #:to #rx"[(]maxLife float[)][)]")
    "\n\n"
    "(import random)\n\n"
    (repository-excerpt "examples/particles/particles.zgal" #:from #rx"^;; A new particle on a ring" #:to #rx"[(]with p #:position position")))
@(zgal-source particles-source)
@(generated-file particles-source "ParticleSim.comp" #:from #rx"^#define particles")

@; -----------------------------------------------------------------------------------------------
@section[#:tag "graphs"]{Graphs}

A @racket[graph] declares a render graph: what C++ supplies (parameters, fixed when the graph is
built, and inputs), the resources it creates, instances of the file's materials bound to them, and
the nodes running them. The compiler generates a struct with a @tt{build()} function adding all of
it to a @tt{RenderGraph}, and returning the ids of its images, buffers and nodes. Everything else a
graph does (ordering, queues, barriers, semaphores, recreating images when the surface resizes) is
the render graph's, as for graphs built in C++.

The scene example's graph:

@(file-excerpt "examples/scene/scene.zgal" #:from #rx"^[(]graph SceneGraph" #:to #rx"readback readback offscreen")

Its app (@secref["apps"]) computes the uniforms, shadows and exposure from its state each frame,
and C++ supplies the meshes.

@racketgrammar*[
 #:literals (graph parameter input per-frame data meshes cpu extern path import-image import-images image-file buffer-file model import-buffer upload
             constant sampler uniform image buffer target surface attachments if color depth material
             dispatch draw-meshes draw readback copy clear fixed extent-of reduce inclusive-scan
             exclusive-scan sort at add min max for range)
 [graph-form (graph Name graph-item ...)]
 [graph-item (parameter name type)
             (input name (per-frame type))
             (input name meshes)
             (input name cpu)
             (input name extern) (input name (extern argument-type ...))
             (input name (data type))
             (input name (per-frame (data type)))
             (input name path)
             (import-image name #:resting resting-state)
             (import-images name #:resting resting-state)
             (image-file name asset #:color color-space)
             (buffer-file name type asset ...+)
             (model name asset Format model-option ...)
             (import-buffer name type)
             (upload name data-input)
             (constant name expression)
             (sampler name sampler-option ...)
             (uniform name type per-frame-input)
             (image name format extent #:frames frames maybe-when)
             (buffer name type count #:frames frames maybe-when)
             (target name target-spec maybe-when)
             (material name Material binding ... material-option ... maybe-when)
             node
             loop]
 [loop (for (loop-clause ...+) loop-body ...+)]
 [loop-clause [name (range from to)] [name (range from to step)] [name data-input]]
 [loop-body node loop]
 [node (dispatch name material maybe-indirect dispatch-option ... node-option ...)
       (draw-meshes name target meshes-input-or-model material maybe-push node-option ...)
       (draw-meshes name target model material #:indirect (at commands index) draw-option ... maybe-push node-option ...)
       (draw name target material vertex-count maybe-push node-option ...)
       (draw name target material #:indirect (at commands index) draw-option ... maybe-push node-option ...)
       (readback name image-or-buffer node-option ...)
       (cpu name cpu-input cpu-option ... node-option ...)
       (copy name source-buffer destination-buffer copy-option ... node-option ...)
       (copy name source-buffer destination-image region-option ... node-option ...)
       (clear name buffer maybe-value node-option ...)
       (extern name extern-input argument ... maybe-when)
       (reduce name source-buffer (at buffer index) #:op op #:count count node-option ...)
       (inclusive-scan name source-buffer destination-buffer #:op op #:count count node-option ...)
       (exclusive-scan name source-buffer destination-buffer #:op op #:count count node-option ...)
       (sort name keys-buffer maybe-values #:count count node-option ...)]
 [maybe-values (code:line) (code:line #:values buffer)]
 [op add min max]
 [count expression (at uint-buffer index)]
 [maybe-value (code:line) (code:line #:value expression)]
 [maybe-indirect (code:line) (code:line #:indirect (at commands index))]
 [dispatch-option (code:line #:push ((push-constant expression) ...)) (code:line #:invocations (x [y [z]]))]
 [maybe-push (code:line) (code:line #:push ((push-constant expression) ...))]
 [draw-option (code:line #:draws expression) (code:line #:count (at uint-buffer index))]
 [region-option (code:line #:source-offset expression) (code:line #:destination-offset (x y))
                (code:line #:extent (width height))]
 [node-option (code:line #:when expression) (code:line #:enabled expression) (code:line #:once boolean)]
 [cpu-option (code:line #:reads (imported-buffer ...)) (code:line #:writes (imported-buffer ...))]
 [copy-option (code:line #:count expression) (code:line #:source-offset expression)
              (code:line #:destination-offset expression)]
 [maybe-when (code:line) (code:line #:when expression)]
 [frames shared per-frame]
 [asset "path" path-input]
 [model-option (code:line #:base-colors name) (code:line #:base-color-factors name)
               (code:line #:draws name) (code:line #:bounds name)]
 [color-space srgb linear]]

Items and nodes appear in any order, and refer to items declared before them. Items share one
namespace (in the graph, and C++'s @tt{Inputs}, @tt{Images} and @tt{Buffers}); nodes have their own
(@tt{Nodes}), so a node can be named like its material.

@subsection{Parameters and inputs}

C++ passes them to @tt{build()} in an @tt{Inputs} struct, by name:

@tabular[
 #:style 'boxed
 #:sep (hspace 2)
 #:row-properties '(bottom-border ())
 (list
  (list @bold{Declaration} @bold{C++ member} @bold{Use})
  (list @tt{(parameter n type)} @elem{@tt{bool}, @tt{int32_t}, @tt{uint32_t} or @tt{float}} "a value fixed when the graph is built: conditions, sizes, push constants")
  (list @tt{(input n (per-frame T))} @tt{zgal::FrameSource<T>} "a function C++ calls each frame with a zgal::FrameInfo: uniforms' sources and push constants")
  (list @tt{(input n meshes)} @tt{std::vector<Handle<Mesh>>} @elem{the meshes a @racket[draw-meshes] node draws})
  (list @tt{(input n cpu)} @tt{RenderGraph::RunFn} @elem{a @racket[cpu] node's work, each frame})
  (list @tt{(input n extern)} @tt{zgal::ExternNodes<Graph>} @elem{a function adding an @racket[extern] node's nodes (@secref["extern"])})
  (list @tt{(input n (data T))} @tt{std::vector<T>} @elem{elements an @racket[upload] puts in a buffer when the graph is built, e.g. a model or a data structure the CPU computed})
  (list @tt{(input n (per-frame (data T)))} @tt{zgal::FrameSource<zgal::Data<T>>} @elem{elements each frame, e.g. an app's @racket[(data T)] state: an @racket[upload] of them is a buffer per frame in flight, sized to them and filled again when they change})
  (list @tt{(input n path)} @tt{std::filesystem::path} @elem{where an asset is, e.g. an @racket[image-file]'s (@secref["assets"])})
  (list @tt{(import-image n #:resting s)} @tt{FrameBinding<Image>} "an image the application owns, returned to its resting state (shader-read-only, general or transfer-src) after each frame")
  (list @tt{(import-images n #:resting s)} @tt{std::vector<FrameBinding<Image>>} @elem{images the application owns, as many as it gives, imported one by one, for an @racket[array] binding (@secref["arrays"]): e.g. textures loaded from files})
  (list @tt{(import-buffer n T)} @tt{FrameBinding<Buffer<T>>} "a buffer the application owns (and fills, maps or reads), with the usage its uses need, one or one per frame"))]

@tt{zgal::FrameInfo} (@tt{include/zgal/Runtime.h}) gives the frame's index (into per-frame
collections), its number, and the graph's surface extent. Missing functions and images are a
contract violation when the graph is built. The graph's materials and nodes keep the functions, so
whatever they use must outlive the graph.

@subsection[#:tag "assets"]{Assets}

Graphs load files when they're built, so C++ needn't. An asset is a path string or a path input:
a relative path is found in the context's asset search paths (@tt{VulkanContextConfig::assetSearchPaths},
@tt{AppConfig::assetSearchPaths}; by default @tt{assets/} beside the executable, where the Makefile's
@tt{zgal-program} copies a program's), in order, or without any relative to the working directory,
by @tt{VulkanContext::findAsset}. Paths given on a generated main's command line are relative to
the working directory. A file that isn't found or can't be read fails @tt{build()}
(@tt{ErrorCode::NotFound} or @tt{InvalidData}), naming the item.

@racket[(image-file name asset #:color color-space)] loads an image (PNG, JPEG, ... through
stb_image) into an RGBA8 image for sampling, imported like an @racket[import-image] resting
shader-read-only: textures and arrays of them bind it, and its size is known when the graph is built
(@racket[(width name)], @tt{(extent-of name)}). Shaders only sample it. The colour space is required,
since it changes what shaders read: @tt{srgb} for colours (photos, albedo), which sampling decodes
to linear, and @tt{linear} for data (normal maps, masks, lookup tables), read as stored. On the web,
8-bit PNGs without interlacing are decoded by the runtime, so transparent texels keep their colour
(browsers' decoders may premultiply alpha), and other images by the browser; under Node only those
PNGs are read.

@(zgal-example image-file-source)
@(generated-file image-file-source "example.zgal.h" #:from #rx"TRY_ASSIGN[(]auto file_albedo" #:to #rx"result.images.mask")

@racket[(buffer-file name type asset ...+)] reads files of binary data (e.g. memory dumps) into a
device-local buffer of @tt{type}'s elements, like an @racket[upload]: each file holds a whole number of
them, laid out as the generated struct is (std430, @secref["types"]), and the buffer holds the files'
elements in order. Shaders read and write it, and the graph knows how many elements it holds only
when it's built, so a @racket[sort] of it takes a @tt{#:count} expression. A file of another size
fails @tt{build()} (@tt{ErrorCode::InvalidData}).

@(zgal-example buffer-file-source)
@(generated-file buffer-file-source "example.zgal.h" #:from #rx"TRY_ASSIGN[(]auto file_sprites" #:to #rx"result.buffers.sprites")

@racket[(model name asset Format)] loads a glTF (@tt{.gltf}, @tt{.glb}) or OBJ (@tt{.obj}, with its
@tt{.mtl}) file: its triangles, with its nodes' transforms baked in, as one mesh of @tt{Format}'s
vertices with a draw per primitive, which @racket[draw-meshes] draws. Each draw's instance index is
its primitive's material's (the file's materials, then a default one for primitives without any),
which vertex shaders read as @racket[instanceIndex]. The file's materials give their base colours: @tt{#:base-colors} declares
their textures (sRGB; white for materials without one), for an @racket[(array texture)] binding,
and @tt{#:base-color-factors} a buffer of their factors (@tt{vec4}s, linear), for a
@racket[(buffer-array vec4)] binding, both indexed by material. Texture coordinates have glTF's
convention (@tt{v} is 0 at the top of the image; OBJ's are flipped to it). For GPU-driven drawing
(@secref["indirect"]), @tt{#:draws} declares a buffer of the draws, as @tt{DrawIndexedCommand}s
(@racket[(import indirect)]), and @tt{#:bounds} one of their bounding spheres (@tt{vec4}s: the
centre, and the radius as @tt{w}), in the model's space.

Models fill the vertex format's attributes by name, converting them to each attribute's type and
storage:

@tabular[
 #:style 'boxed
 #:sep (hspace 2)
 #:row-properties '(bottom-border ())
 (list
  (list @bold{Attribute names} @bold{From} @bold{Types})
  (list @elem{@tt{position}, @tt{pos}} "glTF POSITION, OBJ v" @elem{@tt{vec2}, @tt{vec3}, @tt{vec4} (w 1)})
  (list @tt{normal} "NORMAL, vn" @elem{@tt{vec3}, @tt{vec4} (w 0)})
  (list @elem{@tt{uv}, @tt{uv0}, @tt{texCoord}, @tt{texcoord}} "TEXCOORD_0, vt" @tt{vec2})
  (list @elem{@tt{color}, @tt{colour}} "COLOR_0, OBJ vertex colours" @elem{@tt{vec3}, @tt{vec4}})
  (list @tt{tangent} "TANGENT" @elem{@tt{vec3}, @tt{vec4} (w: the bitangent's sign)}))]

Other attributes need a @racket[#:default], which also fills a named one a file lacks; without
one, a file lacking it fails @tt{build()} (@tt{ErrorCode::InvalidData}). The generated vertex struct
has the conversion, @tt{fromModel}, which @tt{zgal::loadModel} (@tt{include/zgal/Assets.h}) uses
with the wrapper's loaders (@tt{resources/Model.h}).

@(zgal-example model-source #:show '("Paint.vert"))
@(generated-file model-source "example.zgal.h" #:from #rx"static Result<std::vector<Vertex>> fromModel" #:to #rx"^    }$")
@(generated-file model-source "example.zgal.h" #:from #rx"TRY_ASSIGN[(]auto model_ship" #:to #rx"result.buffers.shipFactors")

@subsection[#:tag "graph-expressions"]{Expressions}

@racketgrammar*[
 #:literals (if and or not = < <= > >= + - * / min max float int uint width height)
 [expression number #t #f name
             (if expression expression expression)
             (and expression ...+) (or expression ...+) (not expression)
             (= a b) (< a b) (<= a b) (> a b) (>= a b)
             (+ a b ...+) (- a b ...) (* a b ...+) (/ a b) (min a b) (max a b)
             (float a) (int a) (uint a)
             (vec2 a b) (ivec3 a b c) (code:line ...)
             name.field (.field expression)
             (width image) (height image) (count data-input) (at data-input index)]]

Expressions are typed like GLSL: arithmetic operands share a type (scalars or vectors), and
conditions are bools. Integer literals take the type their context needs, so @tt{(* 6 count)} is a
@tt{uint} when @tt{count} is. A name is a parameter, constant, per-frame input or loop variable
(@secref["loops"]). Fields of structs and swizzles of vectors (@tt{size.x}, @tt{size.xy}) follow names
after dots, @tt{batch.first}, or are functions on other expressions, @racket[(.first (at batches 0))];
@racket[(at input index)] is an element of a data input, checked to exist when it's computed.

Most expressions are computed once, when the graph is built: conditions, constants, extents and
counts. They can use parameters, constants, imported images' sizes and the number of elements C++
gives a data input (@racket[(count input)]). Push constants, compute materials'
@tt{#:invocations} and buffers' counts are computed each frame, so they can also use per-frame
inputs and the current size of the graph's own images, which the graph can recreate (e.g.
surface-relative ones, when the surface resizes).

@subsection{Resources}

@itemlist[
 @item{@racket[sampler] options, all optional: @tt{#:filter} (@tt{linear}, @tt{nearest}),
       @tt{#:mipmap} (@tt{linear}, @tt{nearest}), @tt{#:address} (@tt{repeat},
       @tt{mirrored-repeat}, @tt{clamp-to-edge}, @tt{clamp-to-border}), @tt{#:border}
       (@tt{transparent-black}, @tt{opaque-black}, @tt{opaque-white}), @tt{#:compare} (a
       comparison, e.g. @tt{less-or-equal}, for shadow maps), @tt{#:anisotropy} (@tt{#t} or
       @tt{#f}) and @tt{#:max-lod}. The defaults are @tt{SamplerDesc}'s.}
 @item{@racket[uniform]: a mapped buffer per frame in flight, holding a value of its type that the
       per-frame input computes once a frame, before the first material using it records, however
       many use it. To spare memory traffic, a frame's buffer is only written when the value differs
       from what it holds (so data that doesn't change isn't written again), and the buffers are in
       device-local memory where the device can map it (all memory on unified memory
       architectures, and resizable BAR on discrete GPUs). Shaders only read uniforms.}
 @item{@racket[image]: created by the graph, which derives its usage. The extent is
       @tt{(fixed width height)}, @tt{(surface scale)} (a fraction of the surface, recreated when it
       resizes) or @tt{(extent-of imported-image)}. @tt{#:frames} is required: @tt{per-frame} images
       let frames overlap, @tt{shared} ones save memory.}
 @item{@racket[buffer]: @tt{count} elements of a type, for @tt{buffer} and @tt{buffer-array}
       bindings, in device-local memory. A count using per-frame inputs is computed at the start of
       every frame, and when it changes the graph recreates the buffer (discarding its contents) and
       the materials binding it follow; frames still in flight finish with the previous one. So an
       app's state can size a buffer, e.g. a particle system growing as particles are added. A
       count of 0 is a buffer holding nothing. Give the count to the shaders too (a push
       constant), and dispatch as many invocations.}
 @item{@racket[upload]: a device-local buffer holding a data input's elements, created and
       uploaded when the graph is built. Of a per-frame data input, a buffer per frame in flight,
       sized to the elements each frame (recreated when the count changes, like a @racket[buffer]
       with a per-frame count) and filled through a staging buffer when they change, so data that
       doesn't change isn't copied and frames don't wait for each other. It binds and copies like a graph buffer. An upload given
       no elements is a contract violation (a buffer can't be empty).}
 @item{@racket[import-buffer]: a buffer the application owns, of elements of a type, which it
       creates with the usage its uses need (e.g. @tt{TRANSFER_SRC} to be copied from). Bindings
       and copies use it like a graph buffer, and @racket[cpu] nodes' work can map it.}
 @item{@racket[target]: colour attachments @tt{(color image #:clear (r g b a) #:load clear|load|dont-care
       #:store store|dont-care)} and a depth attachment @tt{(depth image #:clear d ...)}; or the
       swapchain image, @tt{(surface #:clear (r g b a) #:depth format|none)}; or
       @tt{(if condition spec spec)}, where a spec is one of those or @tt{(attachments ...)}.
       Colour attachments default to clearing and storing, and depth to clearing and discarding.}
]

Image formats:

@(format-table)

With @tt{#:when}, an image, buffer, target or material is created only if the condition holds, and
C++ gets a @tt{std::optional} id for an image or buffer. Using one when it wasn't created (e.g. in a
target or binding the condition doesn't cover, or by a node without the condition) is a contract
violation when the graph is built. A program drawing to a window only when it has one gives its
surface target, the materials drawing into it and their nodes @tt{#:when (not headless)}: a
headless graph has no surface.

@subsection{Materials and nodes}

A graph's @racket[material] instantiates a material of the file, giving a resource for each of its
bindings, either @tt{(binding resource)} or just @tt{binding} for a resource of the same name. A
texture or storage image binding takes an image, a sampler binding a sampler, a @tt{buffer} binding
a uniform or (graph or imported) buffer of its type, and a @tt{buffer-array} binding a buffer of its
element type. An @racket[array] binding takes a list of resources of its elements' kind,
@tt{(layers (albedo normals albedo))}, or (for arrays of images) imported images, and the graph
makes its @tt{DescriptorArray}. With @tt{(binding resources #:select indices)}, the elements are
chosen each frame: @tt{indices}, a per-frame data input of uints (@tt{(input indices (per-frame
(data uint)))}), lists the resources' positions, as many as it holds, in any order and repeating
any. The array's buffers grow as needed and nodes using the material declare the chosen elements'
uses before the graph plans the frame (@tt{RenderGraph::setElementSource}), so the number of
elements changes without rebuilding the graph; an index past the resources is a contract
violation. An app chooses them from its state, e.g. with @racket[collect]:

@(zgal-example
  (string-append
   "(compute-material Sum (workgroup 64)\n"
   "  (bindings (sources (array (buffer-array uint))) (total (buffer-array uint)))\n"
   "  (compute (when (= globalInvocationId.x 0)\n"
   "    (write! (at total 0) (sum ([i (range 0 (array-length sources))]) (at (at sources i) 0))))))\n"
   "(graph Sums\n"
   "  (input shown (per-frame (data uint)))\n"
   "  (buffer a uint 1 #:frames shared)\n"
   "  (buffer b uint 1 #:frames shared)\n"
   "  (buffer total uint 1 #:frames shared)\n"
   "  (clear fillA a #:value 1)\n"
   "  (clear fillB b #:value 10)\n"
   "  (material summing Sum (sources (a b) #:select shown) total #:invocations (1))\n"
   "  (dispatch run summing))\n"
   "(app Toggles\n"
   "  (state (showA bool #t) (showB bool #f))\n"
   "  (on (key-pressed a) (with state #:showA (not state.showA)))\n"
   "  (on (key-pressed b) (with state #:showB (not state.showB)))\n"
   "  (run Sums (shown (collect ([i (range (uint 0) 2)]) i\n"
   "                     #:when (if (= i 0) state.showA state.showB)))))\n")
  #:show '("example.zgal.h")
  #:from #rx"elements_summing_sources" #:to #rx"setElementSource")

Its options are
@itemlist[
 @item{@tt{#:push ((field expression) ...)}: a value for every push constant, except those every
       node using the material gives itself (@tt{#:push} on dispatches and draws);}
 @item{graphics materials' pipeline options: @tt{#:cull} (@tt{none}, @tt{back}, @tt{front},
       @tt{front-and-back}), @tt{#:front-face}, @tt{#:blend} (@tt{opaque}, @tt{alpha},
       @tt{premultiplied-alpha}, @tt{additive}), @tt{#:depth-test}, @tt{#:depth-write},
       @tt{#:depth-compare} and @tt{#:topology}, defaulting to @tt{GraphicsPipelineOptions}';}
 @item{compute materials' @tt{#:invocations (x [y [z]])}, the invocations each dispatch covers
       (rounded up to whole workgroups), which is required unless the material is only dispatched
       @tt{#:indirect} or by dispatches giving their own. It's computed each frame, so dispatch
       sizes can follow per-frame inputs (e.g. a reduction over however many elements the CPU has
       this frame) and images' sizes.}
]
A graphics material is built for the formats of the one target its nodes draw into, and every
material is used by a node.

Nodes:
@itemlist[
 @item{@racket[dispatch]: a compute material (a @tt{DispatchNode}). With @tt{#:indirect (at
       commands index)}, as many workgroups as a @tt{DispatchCommand} (@racket[(import indirect)])
       in a graph buffer says, which an earlier node's shader wrote: the GPU sizes its own work
       (@secref["indirect"]). @tt{#:push} and @tt{#:invocations} replace the material's push
       constants (some or all) and invocations for this dispatch, so several dispatches (e.g. a
       loop's, @secref["loops"]) can share a material, and its pipeline (a
       @tt{zgal::OverridingDispatchNode}).}
 @item{@racket[draw-meshes]: draws each mesh of a meshes input or a @racket[model] with a graphics
       material that has a @racket[vertex-input] (a @tt{RenderablesNode}); a model's material reads
       its vertex format. Each draw's first instance is its mesh's instance index
       (@tt{Mesh::create}'s, or the material of a model's primitive). With @tt{#:indirect}, a
       model's commands shaders chose (@secref["indirect"]). Its own @tt{#:push} replaces its
       material's push constants, like a dispatch's.}
 @item{@racket[draw]: draws a vertex count with a graphics material without one, e.g. a fullscreen
       triangle or particles pulled from a buffer, or with @tt{#:indirect}, the @tt{DrawCommand}s
       shaders wrote (@secref["indirect"]). Its own @tt{#:push} replaces its material's push
       constants, like a dispatch's.}
 @item{@racket[readback]: copies an image or a buffer to the CPU each frame (a @tt{ReadbackNode}
       or @tt{BufferReadbackNode}). C++ reads the most recent copy through the generated struct:
       @tt{namePixels(graph)} gives an image's bytes, and @tt{nameContents(graph)} a buffer's
       elements, typed.}
 @item{@racket[cpu]: runs a cpu input each frame, in graph order. @tt{#:reads} and @tt{#:writes} list
       the imported buffers its work reads and writes (the CPU can't map the graph's own buffers,
       which are device local), so the graph orders it with the GPU's uses: e.g. filling a staging
       buffer before a copy reads it. Give such a buffer a copy per frame (a
       @tt{PerFrameHandles}), so the CPU never writes one an earlier frame is still reading.}
 @item{@racket[clear]: fills a buffer with a @tt{uint} value (by default 0) each frame, e.g. to
       reset accumulated sums.}
 @item{@racket[copy] into an image: copies texels from a buffer, tightly packed in the image's
       format, into a region of the image, e.g. a picture the CPU wrote to a staging buffer. By
       default the region is the whole image and the texels start at the buffer's start;
       @tt{#:source-offset} (in the buffer's elements), @tt{#:destination-offset (x y)} and
       @tt{#:extent (width height)} (in texels) choose others, computed when the graph is built.
       The rest of the image keeps its contents. A region outside the image, a buffer too small for
       it, or a source offset that isn't a multiple of 4 bytes and of the texel size (Vulkan's
       rule) is a contract violation when it's recorded.}
 @item{@racket[copy]: copies elements between two buffers (graph, imported or uploaded) of the
       same element type: @tt{#:count} elements (by default, the rest of the source), from @tt{#:source-offset}
       to @tt{#:destination-offset} (in elements, by default 0), computed when the graph is
       built. A range outside either buffer is a contract violation when it's recorded.}
 @item{@racket[extern]: nodes C++ adds (@secref["extern"]).}
 @item{@racket[reduce], @racket[inclusive-scan], @racket[exclusive-scan] and @racket[sort]: the
       compute library's algorithms (@secref["compute-library"]).}
]
@tt{#:when} leaves a node out when the condition is false (C++ gets a @tt{std::optional} id), and
@tt{#:enabled} adds it disabled: disabled nodes still count when the graph allocates resources, and
C++ can enable them later with @tt{graph.setEnabled}. @tt{#:once #t} runs a node in the graph's
first frame only (@tt{RenderGraph::runOnce}), e.g. one-time setup, or a readback of its result;
C++ can run it once more with @tt{graph.runOnce}.

@subsection[#:tag "indirect"]{Indirect dispatches and draws}

GPU-driven work decides on the GPU how much work comes next (how many particles survive, which
objects are visible), and later nodes read that decision without the CPU waiting for it. A shader
writes commands, the structs of @racket[(import indirect)], which are Vulkan's indirect commands
field for field:
@(file-excerpt "dsl/lib/indirect.zgal" #:from #rx"^[(]struct DispatchCommand" #:to #rx"[(]firstInstance uint[)][)]$")
into a graph buffer (the graph gives it indirect usage), and a node names the first,
@tt{#:indirect (at commands index)}. Commands can also come from the CPU: an @racket[upload] or
@racket[buffer-file] (created as indirect buffers), or an imported buffer, which C++ must create
with @tt{VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT} (the graph fails to compile otherwise, as it does
for any imported buffer used in a way its usage doesn't allow):
@itemlist[
 @item{@racket[dispatch] dispatches as many workgroups as its @tt{DispatchCommand} says. Its material
       then needs no @tt{#:invocations}.}
 @item{@racket[draw] draws @tt{#:draws} @tt{DrawCommand}s (by default 1; more need the device's
       @tt{multiDrawIndirect}, which the graph's @tt{requiredFeatures()} then lists), e.g.
       particles a simulation kept, as many instances as there are. A command's
       @tt{firstInstance} other than 0 needs @tt{drawIndirectFirstInstance}. With @tt{#:count (at
       counts index)} it draws as many of them as an element of a buffer of @tt{uint}s says (at most
       @tt{#:draws}): a culling pass can append the commands it keeps and count them.}
 @item{@racket[draw-meshes] of a @racket[model] draws @tt{DrawIndexedCommand}s of its mesh likewise:
       a shader chooses from the model's @tt{#:draws} (e.g. those whose @tt{#:bounds} are in view)
       and copies them into a graph buffer. Their first instances are materials, so the graph's
       @tt{requiredFeatures()} lists @tt{drawIndirectFirstInstance}.}
]
On the web, WebGPU's indirect arguments are the same structs. WebGPU has neither multi-draw nor
indirect counts, so a draw of several commands is a draw call each, and a counted draw always
draws the commands a dispatch copied, those past the count with no instances (as Vulkan devices
without @tt{drawIndirectCount} do). Commands' first instances other than 0 need WebGPU's
@tt{indirect-first-instance}, which the runtime requests when the adapter has it. An imported
buffer of commands needs @tt{GPUBufferUsage.INDIRECT}, which the node checks when it runs.
@(define indirect-source #<<ZGAL
(import indirect)

(compute-material Emit
  (workgroup 1)
  (bindings (draws (buffer-array DrawCommand)) (drawCount (buffer-array uint)))
  (push (visible uint))
  (compute
   (write! (at draws 0) (DrawCommand 3 visible 0 0))
   (write! (at drawCount 0) 1)))

(graphics-material Particle
  (outputs (color vec4))
  (vertex (results #:position (vec4 (float instanceIndex) 0.0 0.0 1.0)))
  (fragment (vec4 1.0)))

(graph Particles
  (buffer draws DrawCommand 1 #:frames per-frame)
  (buffer drawCount uint 1 #:frames per-frame)
  (target screen (surface #:depth none))
  (material emitting Emit draws drawCount #:push ((visible 100)) #:invocations (1))
  (material drawing Particle)
  (dispatch emit emitting)
  (draw particles screen drawing #:indirect (at draws 0) #:count (at drawCount 0)))
ZGAL
)
@(zgal-example indirect-source)

Counts need the device's @tt{drawIndirectCount}, which the context enables where the device has
it. Elsewhere (MoltenVK lacks it) the generated code adapts, adding a node before the draw (named
after it, with @tt{" (limit)"}) that copies the commands with those past the count given no
instances, and drawing all of them:
@(generated-file indirect-source "example.zgal.h" #:from #rx"zgal::addCountedDraw" #:to #rx"zgal::addCountedDraw")
The node limiting the commands follows the draw (@tt{RenderGraph::enableWith}): enabling,
disabling or running the draw once, from the graph, an app or C++ (@tt{nodes.particles}), does the
same to it.

@subsection[#:tag "compute-library"]{The compute library}

Some algorithms come up in GPU-driven work again and again, and take several dispatches and some
care to write well: ZGAL has them as nodes.
@itemlist[
 @item{@racket[reduce] combines a buffer's first elements (@tt{#:count} of them) into one, written to
       an element of a buffer of the same type, @tt{(at buffer index)}: their sum, minimum or maximum
       (@tt{#:op add}, @tt{min} or @tt{max}).}
 @item{@racket[inclusive-scan] writes each element combined with every element before it (a prefix
       sum, minimum or maximum), and @racket[exclusive-scan] what comes before it only (the first
       gets the operator's identity: 0, or the largest or smallest value), e.g. where each item's
       output starts from how many outputs each has. The destination may be the source.}
 @item{@racket[sort] sorts a buffer of @tt{uint} keys in place, ascending and stably (equal keys
       keep their order), and with @tt{#:values}, moves another buffer's elements (@tt{uint}s,
       @tt{int}s or @tt{float}s, e.g. indices) with them: e.g. splats by depth, or particles by
       cell. Other keys map to @tt{uint}s that sort alike: @racket[(import sorting)]'s
       @tt{floatSortKey} and @tt{intSortKey} (and @tt{floatFromSortKey} back).}
]
Scans and reductions take buffers (graph, imported or uploaded) of @tt{uint}s, @tt{int}s or
@tt{float}s. The count is required: an expression computed each frame, or an element of a buffer of @tt{uint}s that an
earlier node wrote on the GPU, @tt{(at counts 0)} (e.g. how many particles a pass kept), which the
kernels read themselves, so nothing waits for the CPU. Elements past the count are left alone.

@(define library-source #<<ZGAL
(graph Compaction
  (input n (per-frame uint))
  (buffer flags uint 4096 #:frames per-frame)
  (buffer offsets uint 4096 #:frames per-frame)
  (buffer kept uint 1 #:frames per-frame)
  ;; ... a pass writing 1 into flags for the items to keep ...
  (exclusive-scan positions flags offsets #:op add #:count n)
  (reduce total flags (at kept 0) #:op add #:count n))
ZGAL
)
@(zgal-example library-source)

Each is one node (a @tt{DispatchSequenceNode}, which the graph orders, enables and synchronizes as
a whole) dispatching materials the compiler instantiates from @tt{dsl/lib/kernels} for the element
type and operator, named @tt{Zgal...}, with a small scratch buffer of their own:
@(generated-file library-source "example.zgal.h" #:from #rx"DispatchSequenceNode>[(].positions" #:to #rx"DispatchSequenceNode>[(].positions")

They reduce, then scan: each workgroup (at most 256, of 256 invocations, taking turns over the
elements when there are more) combines a run of tiles of 256 elements into a partial; a reduction
then combines the partials, and a scan scans them and scans each run again, starting from what
comes before it. Workgroups scan with subgroup operations (so the device's subgroups need arithmetic
operations, and shuffles for minima and maxima, which Metal can't scan natively), then combine the
subgroups' totals in shared memory. With a count on the GPU all 256 workgroups are dispatched,
and those without elements finish at once. A scan in place (from a buffer to itself) reads and
writes its destination, so it binds the buffer once.

A sort is a radix sort, 4 bits a pass, so 8 passes of three dispatches: counting each workgroup's
keys by digit, scanning those counts into where each workgroup's keys of each digit go, and moving
the keys there in order (ranked within subgroups by ballots of their digits' bits, so the device's
subgroups need ballot operations). Keys move between their buffer and a scratch one, and end in their
own. With a count the CPU knows of at most 4096 keys, one workgroup runs every pass in one dispatch
instead, as the 24 dispatches take longer than sorting so few keys. The scratch
buffers hold as many elements as the count, or with a count on the GPU, as the keys' buffer (a
graph or uploaded buffer then, whose size the graph knows).

Scratch buffers are shared by the frames in flight when every buffer the node writes is (a graph
buffer with @racket[#:frames shared], or an uploaded one): each frame's node then waits for the
previous frame's to finish with them anyway, so scratch buffers per frame would only cost memory.
Otherwise there's one per frame, so frames' nodes can overlap.

The benchmark in @tt{tools/bench} (@tt{make run-bench}) times these nodes and draws counted without
@tt{drawIndirectCount} at the size its @tt{--count} gives.

@subsection[#:tag "loops"]{Loops of nodes}

Some graphs have a node per something: per batch of work the application computes, per pass of an
algorithm, per row or layer. A @racket[for] loop repeats its nodes when the graph is built, for each
value of its clauses (nested, the first outermost): @tt{[name (range from to step)]} goes from
@tt{from} up to, and not including, @tt{to}, by @tt{step} (1 by default), all computed when the
graph is built (ints, unless one is a @tt{uint}); @tt{[name input]} goes over the elements of a data
input (@tt{(input name (data T))}), e.g. structs C++ computed. The variables are names in the
loop's nodes' expressions: their conditions, sizes and offsets, and their own push constants and
invocations (@tt{#:push}, @tt{#:invocations}), so the iterations share one material, and pipeline.
A loop holds nodes and loops; declare materials and resources before it. An extern node in a loop
gives its function the iteration's values as arguments (@secref["extern"]).

The compute library's nodes and draws with a @tt{#:count} declare their own materials and scratch
buffers; in a loop, its iterations share them, and each iteration's dispatches push its own values
(counts, indices) and size their own work. Their scratch buffers are sized by what doesn't change
between iterations: a sort whose @tt{#:count} uses the loop's variables gets scratch buffers as large
as its keys' buffer, and a counted draw whose @tt{#:draws} does copies as many commands as its
commands' buffer holds (so the graph must know their sizes: graph buffers and uploads, not imported
buffers or buffer files). Sharing them orders the iterations' nodes one after another.

@(define loops-source #<<ZGAL
(struct Batch
  (first uint)
  (count uint))

(compute-material Settle
  (workgroup 64)
  (bindings (heights (buffer-array float)))
  (push (first uint) (count uint) (rate float))
  (compute
   (let* ([i (+ first globalInvocationId.x)])
     (when (< globalInvocationId.x count)
       (write! (at heights i) (* (at heights i) rate))))))

(graph Batches
  (parameter passes uint)
  (input batches (data Batch))
  (buffer heights float 4096 #:frames shared)
  (material settling Settle heights #:push ((rate 0.5)))
  ;; Each batch, settled `passes` times in order
  (for ([b batches] [pass (range (uint 0) passes)])
    (dispatch settle settling #:push ((first b.first) (count b.count))
              #:invocations (b.count) #:when (> b.count 0)))
  (readback result heights))
ZGAL
)
@(zgal-example loops-source)
@(generated-file loops-source "example.zgal.h" #:from #rx"for [(]const auto& loop_b" #:to #rx"^        }$")

Each iteration's nodes come in order, before the next iteration's, and the graph orders nodes by
their uses in that order, as everywhere: here each batch's passes run one after another. A node in
a loop is one per iteration (whose @tt{#:when} held): C++ finds their ids in @tt{nodes.name}, a
@tt{std::vector<NodeId>}, the graph names them @tt{"name 0"}, @tt{"name 1"}, and so on, and a
looped readback's accessor takes the index, @tt{nameContents(graph, i)}. An app's @tt{#:enabled} enables
or disables them all, and a read event of a looped readback gives each one's copies with its index
(@tt{#:index}). Draws in a loop into one target
each begin by clearing it unless the target loads it (@tt{(color image #:load load)}), so later
iterations keep what earlier ones drew.

@subsection[#:tag "extern"]{Extern nodes}

Some graphs depend on data only C++ has. Loops (@secref["loops"]) cover nodes ZGAL declares; for
anything else, e.g. nodes recording commands ZGAL has no node for, an
@racket[extern] node is where C++ adds nodes itself: its extern input is a function
@tt{zgal::ExternNodes<Graph>}, called by @tt{build()} at the node's place in the graph, with the
@tt{RenderGraph} and the ids so far (every image and buffer, and the nodes before it). It adds nodes
declaring their uses as usual, and returns their ids, which C++ then finds in
@tt{nodes.name} (a @tt{std::vector<NodeId>}, empty when a @tt{#:when} condition was false). Its
nodes can use the file's materials, constructed from their generated classes. The graph orders them
by their uses with the others, so it needs no edges.

An extern input can take arguments, @tt{(input addRow (extern uint Row))}, which each extern node
using it gives, @racket[(extern rows addRow r row)]: expressions computed when the graph is built,
such as a loop's variables, so a loop's extern node adds each iteration's nodes. The function is then
a @tt{zgal::ExternNodes<Graph, uint32_t, Row>}, taking them after the ids (by const reference),
and a looped extern node's @tt{nodes.name} holds every iteration's nodes, in order.

@subsection{The generated struct}

A small graph:

@(define graph-source #<<ZGAL
(struct Settings
  (gain float))

(compute-material Amplify
  (shaders "amplify.comp")
  (workgroup 64)
  (bindings
   (settings (buffer Settings read))
   (samples (buffer-array float read-write)))
  (push
   (count uint)))

(graph Amplifier
  (parameter count uint)
  (parameter verbose bool)
  (input settings (per-frame Settings))
  (input report cpu)
  (uniform settingsBuffer Settings settings)
  (buffer samples float count #:frames shared)
  (material amplify Amplify (settings settingsBuffer) samples
            #:push ((count count))
            #:invocations (count))
  (dispatch amplify amplify)
  (cpu report report #:when verbose))
ZGAL
)
@(define graph-bodies
   (hash "amplify.comp"
         "void main() {\n    uint i = gl_GlobalInvocationID.x;\n    if (i < count()) {\n        samples()[i] *= settings().gain;\n    }\n}\n"))
@(zgal-source graph-source)
@(generated-file graph-source "example.zgal.h" #:bodies graph-bodies #:from #rx"^struct Amplifier" #:to #rx"^    Nodes nodes;")

@tt{requiredFeatures()} collects the device features its materials need (@secref["cpp"]).
@tt{build()} returns a @tt{Result}: creating samplers and uniforms, and loading shaders, can fail.
It creates each material as a @tt{zgal::Instance} or @tt{zgal::PushInstance} of its generated class
(@tt{include/zgal/Runtime.h}), which writes its uniforms and computes its workgroups and push
constants in @tt{update()}, once a frame, so every bind and dispatch that frame uses the same
values. C++ uses the result like any
graph's: @tt{app.setActiveGraph(graph)} or @tt{graph.execute()}, @tt{graph.setEnabled(nodes.name,
...)}, and readbacks.

@; -----------------------------------------------------------------------------------------------
@section[#:tag "apps"]{Apps}

An @racket[app] is an application's behaviour: its state, the handlers computing the next state
from input events and from each frame, and the graph it runs, with per-frame inputs computed from
the state. Handlers are expressions, like shader bodies, and the state is immutable: a handler's
value is the next state, usually a copy with some fields changed (@racket[(with state ...)]). The
compiler generates a C++ class running them, which a generated @tt{main} runs (@secref["main"]), or
C++'s: creating the window (or not), supplying what ZGAL can't (meshes, files, the clock), and
running the app.

The particles example's app:

@(file-excerpt "examples/particles/particles.zgal" #:from #rx"^[(]app ParticlesApp" #:to #rx"0[.]006[)][)][)][)][)]$")

@racketgrammar*[
 #:literals (app parameter state on run key-pressed key-released mouse-pressed mouse-released
             cursor-moved scrolled frame read left right middle data)
 [app-form (app Name app-clause ...)]
 [app-clause (parameter name type)
             (parameter name path)
             (extern (name [parameter : type] ...) : result-type)
             (state (field type initial-value) ...)
             (on event body ...+)
             (run Graph (input expression) ... maybe-enabled maybe-when)]
 [event (key-pressed key ...+)
        (key-released key ...+)
        (mouse-pressed button cursor)
        (mouse-released button cursor)
        (cursor-moved cursor)
        (scrolled offset)
        (frame dt)
        (read readback elements maybe-index maybe-size)]
 [maybe-index (code:line) (code:line #:index i)]
 [maybe-size (code:line) (code:line #:size size)]
 [button left right middle]
 [maybe-enabled (code:line) (code:line #:enabled ((node expression) ...))]
 [maybe-when (code:line) (code:line #:when condition)]]

@subsection{State and handlers}

@itemlist[
 @item{@racket[parameter]s are values C++ gives when it creates the app, of any type, or
       @racket[path]s (@tt{std::filesystem::path}), which the app passes to graphs' path inputs.}
 @item{@racket[extern] declares a function C++ gives (a @tt{std::function} in the app's
       @tt{Inputs}), which bodies call like the file's functions: for what ZGAL can't do itself,
       e.g. reading a dataset or saving results. Its result may be @tt{void}, for effects.}
 @item{@racket[state] declares the state's fields, any type (@tt{bool} too: the state stays on the
       CPU), and their initial values, computed from the parameters.}
 @item{Apps' values can also be @racket[(data T)]: any number of elements of a type laid out like
       a buffer's (@secref["app-data"]).}
 @item{@racket[on] handles an event. Its body names @tt{state}, the event's value (bound to the
       name in the event), the parameters, the file's constants and functions, and the app's
       values (below). Its value is the next state. A body may run effects before it, and a body
       of only effects leaves the state as it is.}
]

The app's values, which every body can read:
@tabular[
 #:style 'boxed
 #:sep (hspace 2)
 #:row-properties '(bottom-border ())
 (list
  (list @bold{Value} @bold{Type} @bold{What})
  (list @tt{framesInFlight} @tt{uint} "the number of frames in flight")
  (list @tt{surfaceSize} @tt{vec2} "the active graph's surface extent")
  (list @tt{frameNumber} @tt{uint} "the frame being drawn, counting the frames the app begins from 0")
  (list @tt{headless} @tt{bool} "whether the app runs without a window")
  (list @tt{capture} @tt{bool} "whether the last frame is captured (headless only)")
  (list @tt{frameLimit} @tt{uint} "the frames the app runs, or 0 until the window closes or it quits"))]
@tt{headless}, @tt{capture} and @tt{frameLimit} are how the app is run, which C++ gives as a
@tt{zgal::RunMode} in the app's @tt{Inputs} (only when a body reads them), and a generated
@racket[main] chooses from the command line. With them an app can script a smoke test, e.g. change
the frames in flight a third of the way through: @racket[(when (= frameNumber (/ frameLimit 3))
(set-frames-in-flight! 3))].

Events:
@tabular[
 #:style 'boxed
 #:sep (hspace 2)
 #:row-properties '(bottom-border ())
 (list
  (list @bold{Event} @bold{Value} @bold{When})
  (list @tt{(key-pressed key ...)} "" "one of the keys is pressed")
  (list @tt{(key-released key ...)} "" "one of the keys is released")
  (list @tt{(mouse-pressed button cursor)} @elem{the cursor, a @tt{vec2}} "the button is pressed")
  (list @tt{(mouse-released button cursor)} @elem{the cursor, a @tt{vec2}} "the button is released")
  (list @tt{(cursor-moved cursor)} @elem{the cursor, a @tt{vec2}} "the cursor moves")
  (list @tt{(scrolled offset)} @elem{the offset, a @tt{vec2} in scroll steps, y up} "the user scrolls")
  (list @tt{(frame dt)} @elem{the time since the last frame, a @tt{float} in seconds} "each frame, before drawing")
  (list @tt{(read readback elements)} @elem{the elements a buffer readback copied, or the texels an image readback copied, @racket[(data T)]} "a readback's copy has finished (polled before each frame, without waiting)"))]
A readback in a loop is one per iteration: its read event binds which one copied with @tt{#:index i}
(a @tt{uint}, the readback's index in @tt{nodes.name}), and handles each one's copies. An image
readback's texels are one per pixel, row by row, each holding the pixel's bits: a @tt{uint} for
the 8-bit RGBA and BGRA formats (the first channel in the low byte), @tt{float}, @tt{vec2} or
@tt{vec4} for @tt{r32f}, @tt{rg32f} and @tt{rgba32f}, @tt{uvec2} for @tt{rgba16f} (two halves per
@tt{uint}), and so on (@tt{r32ui} and @tt{r32i} as they are, @tt{r8} a @tt{uint8}, @tt{rg8} and
@tt{r16f} a @tt{uint16}, @tt{rg16f} a @tt{uint}). @tt{#:size size} binds the image's size when it
was copied (a @tt{uvec2}). Imported images' formats aren't known, so only graph images and image
files (RGBA8) have read events.
Cursor positions are in [-1, 1] across the window, y down, like Vulkan's clip space. Keys are
@tt{a}-@tt{z}, @tt{0}-@tt{9}, @tt{space}, @tt{escape}, @tt{enter}, @tt{tab}, @tt{backspace},
@tt{left}, @tt{right}, @tt{up}, @tt{down}, @tt{minus}, @tt{equal}, @tt{keypad-add},
@tt{keypad-subtract} and @tt{f1}-@tt{f12}. An event is handled once: a key belongs to one handler.

Effects, which only handlers have:
@itemlist[
 @item{@racket[(print! value ...)] prints strings and scalars, then a newline.}
 @item{@racket[(set-frames-in-flight! n)] changes the number of frames in flight, at the next
       frame boundary.}
 @item{@racket[(resize! width height)] (@tt{uint}s) resizes the window, or headless, the graphs'
       surface extent (@tt{zgal::drawFrame} applies it after the handlers), which surface-relative
       images follow.}
 @item{@racket[(quit!)] asks the app to stop: @tt{zgal::run} returns.}
 @item{@racket[(run-once! node)] runs a @tt{#:once #t} node again, in the next frame its graph
       draws (in each graph the app runs that has it; @tt{RenderGraph::runOnce}), e.g. to compute
       a phase's results again.}
]

@subsection[#:tag "app-data"]{Data}

@racket[(data T)] values are elements an app holds without a size known in advance: a
readback's results, what an extern function computes, what a graph uploads. They're immutable and
shared (@tt{zgal::Data<T>} in C++), so copying one with the state costs nothing, and data that hasn't
changed is the same data, which graphs don't upload again. Bodies count them with
@racket[(count d)], read an element with @racket[(at d i)], and start with none,
@racket[(empty-data T)]; extern functions take and give them (by reference), e.g. to sort
results. A graph's @tt{(input name (per-frame (data T)))} takes them each frame.

@racket[(collect ([name source] ...) expression)] makes data: an element of the expression's value
for each step of its clauses, nested (the first outermost), as @racket[fold]'s. A clause goes over a
range, @tt{[i (range from to)]}, or a data value's elements, @tt{[x d]} (computed once, before the
loops, so it can't use the clauses' names; later clauses' ranges can use @tt{x}). With
@tt{#:when condition}, only the steps where it holds give elements, so collect maps, filters and
builds data:
@racketblock[
(on (key-pressed f)
  (with state #:visible (collect ([p state.particles]) p.position #:when (> p.age 0.0))))
]
Its elements are numbers, vectors, matrices or structs (not bools), so graphs can upload them.

@subsection{The graph}

@racket[run] names a graph of the file and computes its inputs: a parameter's or path input's
expression once, when the app is created, from the app's parameters; a per-frame input's each frame, from the state too.
@tt{#:enabled} computes, each frame, whether nodes run, e.g. a pass the user toggles. Whatever the
app doesn't compute (meshes, cpu and extern inputs, imported images and buffers, and any parameter,
path or per-frame input it leaves alone) C++ gives, through the app's inputs.

@subsubsection[#:tag "phases"]{Phases}

An app can run several graphs, one each frame: the first whose @tt{#:when} condition holds, or
otherwise the last (which has none). So work happens in phases, e.g. precomputing a table, then
drawing with it. Within a graph, a node with @tt{#:once #t} runs in the graph's first frame only.
Results come back as @racket[read] events, without stalling the frames, and the state carries them
to the next phase:

@(define phases-source #<<ZGAL
(compute-material Measure
  (workgroup 64)
  (bindings (sizes (buffer-array uint)))
  (compute (write! (at sizes globalInvocationId.x) (* globalInvocationId.x 2))))

(compute-material Draw
  (workgroup 64)
  (bindings (sizes (buffer-array uint)) (shapes (buffer-array float)))
  (push (count uint))
  (compute
   (let* ([i globalInvocationId.x])
     (when (< i count) (write! (at shapes i) (float (at sizes i)))))))

(graph Precompute
  (buffer sizes uint 64 #:frames shared)
  (material measuring Measure sizes #:invocations (64))
  (dispatch measure measuring #:once #t)
  (readback readSizes sizes #:once #t))

(graph Render
  (input sizes (per-frame (data uint)))
  (upload sizeBuffer sizes)
  (buffer shapes float (count sizes) #:frames per-frame)
  (material drawing Draw (sizes sizeBuffer) shapes
            #:push ((count (count sizes))) #:invocations ((count sizes)))
  (dispatch draw drawing))

(app Staged
  (state
   (sizes (data uint) (empty-data uint))
   (ready bool #f))
  (on (read readSizes copied) (with state #:sizes copied #:ready #t))
  (run Precompute #:when (not state.ready))
  (run Render (sizes state.sizes)))
ZGAL
)
@(zgal-source phases-source)
@(generated-file phases-source "example.zgal.h" #:from #rx"^    Result<void> beginFrame" #:to #rx"^    }$")

C++ gives such an app a render graph per graph (@tt{Name::Graphs}), and draws the one
@tt{renderGraph()} returns after each @tt{beginFrame()}, as @tt{zgal::run} does.

@subsection{The generated class}

A small app:

@(define app-source #<<ZGAL
(graphics-material Fill
  (push (shade float))
  (outputs (color vec4))
  (vertex (results #:position (vec4 (- (* (vec2 (float (bitand vertexIndex 1)) (float (shr vertexIndex 1))) 4.0) 1.0) 0.0 1.0)))
  (fragment (vec4 shade)))

(graph Canvas
  (input shade (per-frame float))
  (target screen (surface #:depth none))
  (material fill Fill #:push ((shade shade)))
  (draw paint screen fill 3))

(app Dimmer
  (parameter start float)
  (state
   (shade float start)
   (fading bool #f))
  (on (key-pressed f) (with state #:fading (not state.fading)))
  (on (key-pressed escape) (quit!))
  (on (frame dt)
    (if state.fading (with state #:shade (max 0.0 (- state.shade (* 0.5 dt)))) state))
  (run Canvas (shade state.shade)))
ZGAL
)
@(zgal-source app-source)
@(generated-file app-source "example.zgal.h" #:from #rx"^class Dimmer" #:to #rx"^    // Runs the handler")

A class @tt{Name} has
@itemlist[
 @item{@tt{struct State}, and @tt{state()} and @tt{setState()} (for tests driving the app);}
 @item{@tt{struct Inputs}: the parameters, a @tt{zgal::Timestep timestep} (with a frame handler),
       a @tt{zgal::RunMode mode} (when a body reads how the app is run), the extern functions, and
       the graph's inputs the app doesn't compute;}
 @item{@tt{static Result<std::unique_ptr<Name>> create(context, graph, inputs)}, which computes the
       initial state and builds the graph into the @tt{RenderGraph} (without @racket[run], the app
       has no graph of its own: C++ builds one, and reads the state). An app running several graphs
       takes @tt{Name::Graphs}, a @tt{RenderGraph*} per graph (named like the graph, lower case
       first: @tt{.precompute}), instead;}
 @item{@tt{handle(const zgal::Event&)}, running the event's handler, and @tt{Result<void>
       beginFrame()}, running the handlers of readbacks' finished copies and the frame handler,
       choosing the graph and computing the enables (it fails if reading a copy does);}
 @item{@tt{ids()} (the graph's ids; with several graphs, @tt{precomputeIds()} and so on),
       @tt{renderGraph()} (the graph to draw this frame), @tt{context()} and @tt{quitRequested()};}
 @item{@tt{takeResize()}, the size a handler last asked for with @racket[resize!] (once), and
       @tt{setSurfaceExtent(extent)}, setting every graph's, for headless loops applying it;}
 @item{@tt{static DeviceFeatures requiredFeatures()}, what its graphs need of the device.}
]

The clock is C++'s choice, made explicit: @tt{zgal::Timestep::realTime(maxStep)} advances by the
time since the last frame (at most @tt{maxStep}, so a stall doesn't make the app jump; 0 on the
first frame), and @tt{zgal::Timestep::fixed(step)} by a fixed step, so headless runs are
deterministic. So is how it's run: @tt{zgal::RunMode::window(frameLimit)}, or
@tt{zgal::RunMode::headless(frameLimit, capture)}.

@subsection{Running an app}

@tt{include/zgal/App.h} holds what the generated class uses (events, @tt{Timestep}, @tt{RunMode}) and knows
nothing of windows. @tt{include/zgal/Window.h} connects an app to a @tt{VulkanApp}:
@tt{zgal::run(vulkanApp, app)} sends the window's input to the app's handlers and draws frames,
each after @tt{beginFrame()} (drawing the graph the app chose), until the window closes or the app
quits; @tt{zgal::drawFrame} draws one, for headless loops. Without a @tt{VulkanApp}, C++ calls
@tt{beginFrame()} and executes @tt{renderGraph()} itself. A generated @tt{main} does all of this
(@secref["main"]); the simple example's handwritten one, which builds its graph in C++, creates its
app and runs it:

@(file-excerpt "examples/simple/simple_example.cpp" #:from #rx"TRY_ASSIGN[(]auto simpleApp, simple::SimpleApp::create" #:to #rx"^    [}][)][)];")
@(file-excerpt "examples/simple/simple_example.cpp" #:from #rx"return zgal::run[(]app, [*]simpleApp[)];" #:to #rx"return zgal::run")

@; -----------------------------------------------------------------------------------------------
@section[#:tag "main"]{Programs}

A @racket[main] makes a file a program: the compiler writes its @tt{main} too
(@tt{name.zgal.main.cpp}), which reads the command line into the app's parameters, creates the
context, the window (or not) and the app's graphs, runs the app, and writes its capture. A program
whose app needs nothing from C++ is the @tt{.zgal} file alone; one that does gives it from a small C++
file of its own (@secref["main-inputs"]).

@racketgrammar*[
 #:literals (main about window timestep capture controls flag option positional)
 [main-form (main App main-clause ...)]
 [main-clause (about "what the program does")
              (window "title" width height)
              (timestep #:max seconds #:fixed seconds)
              (capture readback maybe-to)
              (controls "line" ...)
              (flag parameter "--name" "help")
              (option parameter "--name" "help" #:default value maybe-range maybe-given)
              (positional parameter "help" maybe-range)]
 [maybe-to (code:line) (code:line #:to path-parameter)]
 [maybe-range (code:line) (code:line #:range (low high))]
 [maybe-given (code:line) (code:line #:given bool-parameter)]]

@itemlist[
 @item{@racket[window] gives the program a window, with its title and size. Without one, the
       program is only headless (it doesn't need a window system), and runs one frame unless told
       otherwise.}
 @item{@racket[timestep] chooses the frame handler's clock, required when the app has one: real
       time interactively, at most @tt{#:max} seconds a frame, and @tt{#:fixed} seconds a frame for
       a number of frames, so those runs are deterministic.}
 @item{@racket[capture] names an image readback (of an @tt{rgba8} or @tt{rgba8-srgb} image) whose
       last frame is written to a PNG file: the one @tt{--capture} names, or with @tt{#:to}, the
       path parameter an option sets.}
 @item{@racket[controls] are printed when the program runs interactively.}
 @item{@racket[flag], @racket[option] and @racket[positional] set the app's parameters from the
       command line: a flag sets a @tt{bool} parameter when given; an option takes a value
       (@tt{int}, @tt{uint}, @tt{float} or @tt{path}), with a default and optionally a range, and
       @tt{#:given} also sets a @tt{bool} parameter when it's given; positionals are required, in
       order. Paths are relative to the working directory. Each parameter is set by one option at
       most.}
]

Every program also has the built-in options:
@tabular[
 #:style 'boxed
 #:sep (hspace 2)
 #:row-properties '(bottom-border ())
 (list
  (list @bold{Option} @bold{What})
  (list @tt{--frames N} "run N frames, then exit: headless (with a window, unless --window), on the fixed clock")
  (list @tt{--capture FILE} @elem{write the capture's last frame (with a @racket[capture] without @tt{#:to})})
  (list @tt{--capture-frames N,...} @elem{write those frames' captures instead, as @tt{FILE/N.png} (@tt{FILE} a directory), running as many frames as the last needs unless @tt{--frames} says more})
  (list @tt{--size WxH} "the window's size, or headless the surface's (with a window)")
  (list @tt{--window} "with --frames, run in the window")
  (list @tt{--validation} "enable validation and synchronization validation (the run fails without either): any message fails the run")
  (list @tt{--plan} "print the render graph's plan")
  (list @tt{--timings} @elem{print each node's GPU time at the end (@tt{RenderGraph::setTimingEnabled}): the median, fastest and slowest frame's})
  (list @tt{--help} "print the usage"))]
The app reads how it was run as @tt{headless}, @tt{capture} and @tt{frameLimit} (@secref["apps"]),
e.g. to script a smoke test. The exit status is 0, 1 for an error or validation messages, and 2 for
a bad command line.

@(define main-source #<<ZGAL
(compute-material Fill
  (workgroup 8 8)
  (bindings (canvas (storage-image write)))
  (push (level float))
  (compute (store! canvas (ivec2 globalInvocationId.xy) (vec4 level 0.0 0.0 1.0))))

(graph Painting
  (parameter level float)
  (parameter side uint)
  (image canvas rgba8 (fixed side side) #:frames shared)
  (material fill Fill canvas #:push ((level level)) #:invocations (side side))
  (dispatch paint fill)
  (readback result canvas))

(app Painter
  (parameter level float)
  (parameter side uint)
  (parameter destination path)
  (run Painting (level level) (side side)))

(main Painter
  (about "Paints a red square")
  (option level "--level" "how red" #:default 1.0 #:range (0.0 1.0))
  (option side "--side" "the square's side, in pixels" #:default 64)
  (positional destination "the PNG file to write")
  (capture result #:to destination))
ZGAL
)
@(zgal-source main-source)
@(generated-file main-source "example.zgal.main.cpp")

The particles example is a program, without C++ of its own: a window, its clock, an option for
the number of particles and the smoke test's capture (@tt{--frames 120 --capture smoke.png}), whose
script (changing the frames in flight) is in its app's frame handler:

@(file-excerpt "examples/particles/particles.zgal" #:from #rx"^[(]main ParticlesApp" #:to #rx"frames in flight\"[)][)]$")

@tt{include/zgal/Main.h} holds what generated mains use: the command line (@tt{zgal::CommandLine},
@tt{Launch}), counting validation messages, captures and running a headless program
(@tt{zgal::runHeadless}); @tt{include/zgal/Window.h} runs one with a window (@tt{zgal::runWindowed}).

@subsection[#:tag "main-inputs"]{What the program's code gives}

What the command line can't give the app (its @racket[extern] functions, its graphs' inputs it
doesn't compute, such as meshes, and parameters no option sets), the program's own C++ does: the
header then declares @tt{struct MainInputs}, holding them, and @tt{Result<MainInputs>
mainInputs(VulkanContext&)}, which a C++ file of the program defines, and which the generated main
calls once the context exists:

@(define hooked-source #<<ZGAL
(app Reporter
  (parameter verbose bool)
  (parameter limit uint)
  (extern (report [x : uint]) : void)
  (state (n uint 0))
  (on (key-pressed space) (when verbose (report limit)) state))

(main Reporter
  (flag verbose "--verbose" "report"))
ZGAL
)
@(zgal-source hooked-source)
@(generated-file hooked-source "example.zgal.h" #:from #rx"^// What the program's C[+][+] gives" #:to #rx"^Result<MainInputs>")

@verbatim{
#include "example.zgal.h"

Result<example::MainInputs> example::mainInputs(VulkanContext&) {
    return MainInputs{.limit = 10, .report = [](uint32_t x) { std::cout << x << std::endl; }};
}
}

On the web, the program's TypeScript gives them: its module declares @tt{interface MainInputs},
and @tt{example.inputs.ts}, beside @tt{example.zgal}, exports @tt{mainInputs(context)}, returning
them or a promise of them, which the generated main (@tt{example.zgal.main.ts}) imports. The
@tt{.ts} files beside the @tt{.zgal} file are the program's own TypeScript, which
@tt{zgal-web-program} builds with it:

@(generated-file hooked-source "example.zgal.ts" #:web? #t #:from #rx"^// What the program's TypeScript gives" #:to #rx"^}")

@verbatim{
import type * as zgal from './zgal/index.ts';
import type { MainInputs } from './example.zgal.ts';

export function mainInputs(_context: zgal.Context): MainInputs {
  return { limit: 10, report: (x) => console.log(x) };
}
}

The generated main compiles the wrapper's implementation (@tt{VK_WRAP_UTIL_IMPL}), stb_image's
and stb_image_write's (and the model loaders', if the program loads models), so the program's
other files don't.

@; -----------------------------------------------------------------------------------------------
@section[#:tag "glsl"]{GLSL bodies}

A material's shaders can be written in GLSL instead, listed in its @racket[shaders] clause, with
the interface still in ZGAL. Accesses can't be inferred from GLSL, so its storage bindings declare
theirs. A generated shader is then, in order,
@itemlist[#:style 'ordered
 @item{@tt{#version 450}, @tt{GL_GOOGLE_cpp_style_line_directive} (for @tt{#line} with file names)
       and @tt{#include "bindless.glsl"} (the universal descriptor set's arrays);}
 @item{every struct in the file;}
 @item{the push constant block, @tt{materialPush}: a @tt{uint} index per binding, then the push
       fields;}
 @item{one buffer block declaration per buffer type (shared by bindings of the same type), all
       @tt{readonly} unless a buffer binding is written;}
 @item{an accessor macro per binding and push field;}
 @item{the stage's inputs and outputs: vertex inputs (named @tt{in} followed by the attribute's
       name, e.g. @tt{inPosition}) and varyings for vertex shaders, varyings and outputs for
       fragment shaders, the local size for compute shaders;}
 @item{the body, from its first line, numbered as itself (glslc reports its errors at its lines).}
]
So a body starts with its own code: no @tt{#version}, push constant block, bindings, inputs or
outputs (a @tt{#version} line is an error). It can define functions, constants and macros, and
@tt{#include} files beside it.

Bindings and push constants are read through accessors, function-like macros
(@tt{#define hdr() (bindlessTextures[materialPush.hdr])}), so they only expand where the name is
followed by @tt{(}: the push constant block's members, struct members and local variables can share
their names (@tt{SceneUniforms scene = scene();}). Combine a texture and a sampler as GLSL does,
@tt{texture(sampler2D(albedo(), albedoSampler()), uv)}, and write buffers through their accessors,
@tt{particles()[i].velocity = v}.

@; -----------------------------------------------------------------------------------------------
@section[#:tag "cpp"]{The generated C++}

The header includes the wrapper's material headers and declares, in file order, the structs, the
vertex formats, the functions and constants that compile to C++ (@secref["functions"]), a class
per material, then a struct per graph. Using the material's members needs nothing but the
header.

@subsection{A material's class}

A material @tt{Name} becomes @tt{class Name : public GraphicsMaterial} (or @tt{ComputeMaterial}),
with
@itemlist[
 @item{@tt{struct Bindings}: a @tt{FrameBinding<DescriptorEntry>} per binding, by name. Give each
       one an entry, either shared by every frame or one per frame: a missing entry is a contract
       violation when the material is constructed.}
 @item{@tt{struct Push} (with push constants): the push fields, laid out as in the shaders.}
 @item{@tt{Shaders} (@tt{zgal::GraphicsShaders} or @tt{zgal::ComputeShaders} of the material,
       from @tt{zgal/Material.h}) and @tt{static Result<Shaders> loadShaders(VulkanContext&)},
       which loads the material's shaders from the shader library. Only the material can make a
       @tt{Shaders}, and its constructor takes one, so it can only be built from the shaders
       generated with its interface.}
 @item{Graphics materials: @tt{kColorOutputs}, the number of outputs, and with a
       @racket[vertex-input], @tt{static VertexLayout vertexLayout()}.}
 @item{Compute materials: @tt{kWorkgroupSizeX}, @tt{Y} and @tt{Z}, and
       @tt{static glm::vec3 workgroupsFor(x, y = 1, z = 1)}.}
 @item{@tt{static DeviceFeatures requiredFeatures()}: the device features its shaders need beyond
       the wrapper's (8- and 16-bit integers), empty for most materials. Add them to the context's,
       @tt{config.features.add(Name::requiredFeatures())}, before creating it; a graph's
       @tt{requiredFeatures()} collects its materials'.}
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
 @item{a context without the device features the material needs (in @tt{loadShaders}, before
       creating shader modules that use them);}
 @item{a binding without an entry (when constructed);}
 @item{a graphics material whose formats have a different number of colour attachments than its
       outputs, or options with a vertex layout (when constructed);}
 @item{an entry of the wrong kind, e.g. a sampler given for a texture, which would index the wrong
       array (whenever the material is bound, for that frame's entries).}
]

@; -----------------------------------------------------------------------------------------------
@section[#:tag "web"]{The web (WebGPU)}

ZGAL also compiles for the web, to WebGPU: @tt{@literal{--web-out} DIR} writes a TypeScript module,
@tt{FILE.zgal.ts}, a WGSL module per material shader, named after it (@tt{Lit.frag.wgsl}), and for
a program its main, @tt{FILE.zgal.main.ts}, and page, @tt{FILE.zgal.html}. This section describes how the shaders map
to WebGPU, which has no bindless descriptors and lacks some of what Vulkan has, then the TypeScript.

@subsection{Shaders}

@itemlist[
 @item{A material's bindings are a bind group of their own (group 0): the @italic{i}th binding is
       @tt{@"@"binding(i)}. Its push constants are a uniform buffer after them (@tt{zgalPush}, of
       struct @tt{ZgalPush}), so they're scalars, vectors and matrices: uniform buffers pad arrays
       and structs differently. A module declares only the bindings its stage uses.}
 @item{WGSL has no 8- and 16-bit integers, and has atomics only on @tt{atomic<u32>} variables. A
       buffer or shared variable holding small integers, or one that atomics act on, is declared as
       32-bit words and read and written through generated functions at its std430 offsets, so the
       memory is laid out as the C++ and GLSL lay it out. A value filling whole words at an aligned
       offset (e.g. a struct of four @racket[uint8]s at a multiple of 4) is written a word at a time;
       otherwise each small integer's bytes are changed atomically, as other invocations may be
       writing the rest of its word (the words are then @tt{atomic<u32>}).}
 @item{Atomics WGSL lacks are compare-exchange loops: signed minimums and maximums, and float adds.
       A compare-exchange is retried until it doesn't fail spuriously, as GLSL's doesn't.}
 @item{Subgroup scans other than adds and multiplies are shuffles, ballot counts count bits, and
       @racket[subgroupId] and @racket[subgroupCount] assume invocations form subgroups in order of
       their local index.}
 @item{A texture compared with depths (@racket[sample-compare]) is a depth texture, and its sampler a
       comparison sampler, which only compares: sampling the texture gives @tt{(depth, 0, 0, 1)}, as
       Vulkan does, but with another sampler.}
 @item{A storage image's texel format is part of its WGSL type: modules are written with
       @tt{rgba8unorm}.}
 @item{WebGPU's vertex shaders can't write: buffers and storage images a vertex shader uses must be
       read-only there, so a vertex shader that writes one (@racket[write!], @racket[store!] or an
       atomic) compiles but fails with a WebGPU validation error when the graph creates the
       material's bind group layout.}
 @item{Names WGSL reserves (e.g. @tt{target}, @tt{type}) are spelled with a @tt{zgal_} prefix.}
 @item{An array of textures is a @tt{texture_2d_array}, a layer per element, so its elements are
       only used in texture operations (@racket[sample], @racket[sample-lod], @racket[fetch] and
       @racket[texture-size]), and @racket[array-length] counts its layers. Bound to a model's base colours, it is their layers (a texture array the model's
       images are resized into, the largest's size); bound to image files of one colour space,
       they are copied into one when the graph is built, resized the same way.}
]

GLSL bodies, @racket[extern-glsl] functions and other arrays of bindings (of buffers or storage
images, or chosen with @racket[#:select] or from imports) have no WebGPU equivalent, so compiling
them for the web reports @secref["error-not-on-web"]. The example's TypeScript follows in
the next section.

@zgal-example[#:web? #t #:show '("Copy.comp.wgsl")
"(struct Pixel (r uint8) (g uint8) (b uint8) (a uint8))

(compute-material Copy
  (workgroup 64)
  (bindings
   (source (buffer-array Pixel))
   (destination (buffer-array Pixel)))
  (push (brighten uint))
  (compute
   (let* ([i globalInvocationId.x]
          [p (at source i)])
     (write! (at destination i) (with p #:r (uint8 (+ p.r brighten)))))))"]

@subsection{TypeScript}

The TypeScript module imports ZGAL's runtime, @tt{web/zgal}, as @tt{./zgal/index.ts}: copy that
directory beside it. It's written in TypeScript's erasable syntax, so Node (24 or later) runs it as
it is, and browsers after @tt{tsc} or a bundler strips the types. The module exports:

@itemlist[
 @item{per struct, an interface of its values (numbers, arrays of numbers for vectors, arrays of
       columns for matrices) and a constant of the same name describing its std430 layout, which
       the runtime encodes and decodes buffers' elements with;}
 @item{per vertex format, a constant of the same name describing its attributes (offsets, formats,
       defaults and what models fill them from), which models' vertices are laid out with, and
       @tt{encodeVertices} lays out a meshes input's;}
 @item{per material, a description: its bindings (with the accesses and stages inferred from its
       shaders), its push constants' offsets, and its WGSL, which the runtime builds bind group
       layouts and pipelines from;}
 @item{per graph, an interface of its inputs (@tt{BrightenInputs}: parameters and data as values,
       per-frame inputs as functions of the frame, cpu and extern inputs as functions) and a class
       whose @tt{build(context, graph, inputs)} adds its resources, materials and nodes to a
       @tt{RenderGraph}, returning their ids, with an accessor per readback;}
 @item{the file's functions and constants the CPU can run (those the C++ has), as functions and
       constants;}
 @item{per app, an interface of its state and of its inputs, and a class like the C++ one (see
       below).}
]

@zgal-example[#:web? #t #:show '("example.zgal.ts") #:from #rx"^// graph Brighten"
"(struct Pixel (r uint8) (g uint8) (b uint8) (a uint8))

(compute-material Copy
  (workgroup 64)
  (bindings
   (source (buffer-array Pixel))
   (destination (buffer-array Pixel)))
  (push (brighten uint))
  (compute
   (let* ([i globalInvocationId.x]
          [p (at source i)])
     (write! (at destination i) (with p #:r (uint8 (+ p.r brighten)))))))

(graph Brighten
  (parameter count uint)
  (input pixels (data Pixel))
  (input amount (per-frame uint))
  (upload source pixels)
  (buffer destination Pixel count #:frames shared)
  (material copy Copy (source source) (destination destination)
            #:push ((brighten amount)) #:invocations (count))
  (dispatch copy copy)
  (readback result destination))"]

A @tt{zgal.Context} holds the device (@tt{Context.create()} takes @tt{navigator.gpu} in a browser;
under Node, @tt{createNodeContext} from @tt{zgal/node.ts} uses Dawn), and a @tt{zgal.RenderGraph}
runs the nodes each @tt{execute()}, as the C++ graph does. The web tests build and run a graph like
this:

@file-excerpt["web/tests/graphs.test.ts" #:from #rx"^before" #:to #rx"^}\\);"]

The runtime follows the C++ graph's behaviour where WebGPU allows, with these differences:
@itemlist[
 @item{WebGPU synchronizes between passes itself, so the runtime records every node into one command
       encoder in insertion order, without barriers, semaphores or other queues.}
 @item{WebGPU keeps resources a submitted frame uses alive, so every resource is shared between
       frames (@racket[#:frames] is ignored) and two frames at most are in flight.}
 @item{Push constants are a uniform buffer per node using the material, written before each frame.}
 @item{Buffers and images are created with every usage their format allows.}
 @item{A window's surface is drawn through an sRGB view of the canvas's format, so shaders output
       linear values as on Vulkan.}
 @item{Clip space is Vulkan's (y down) in shaders too: vertex shaders' positions have their y
       negated for WebGPU's, which keeps triangles, and so their winding, where they are on the
       screen.}
 @item{WebGPU can't cull both faces, so a material with @tt{#:cull front-and-back} keeps its
       triangles and draws them with a fragment stage that discards every fragment and writes no
       colour in place of its own: its vertex shader runs, as Vulkan's does (though on the web it
       can't write, see above), and nothing is drawn or written by its fragment shader. Lines
       and points are drawn, as Vulkan culls only triangles.}
]

Functions, constants and apps' bodies compute what the C++ does: JavaScript's numbers are doubles,
so the runtime (@tt{math.ts}, and @tt{glsl.ts} for the built-in functions, as GLM computes them)
rounds floats to 32 bits after each operation and wraps integers at 32 bits. Vectors are arrays of
numbers and matrices arrays of columns. Floats given to generated code (functions' arguments,
parameters, per-frame inputs) must be 32-bit ones (@tt{Math.fround}), as C++'s @tt{float}
parameters would make them.

An app's class is created with @tt{await App.create(context, graph, inputs)} (or a graph per
@racket[run], @tt{AppGraphs}), its @tt{inputs} giving its parameters, @tt{timestep}
(@tt{zgal.Timestep.realTime(max)} or @tt{fixed(step)}), @tt{mode} (@tt{zgal.RunMode.window(frames)} or
@tt{headless(frames, capture)}), extern functions and the graph inputs the app doesn't compute. As
in C++, each frame calls @tt{beginFrame()} then executes @tt{renderGraph()}; @tt{handle(event)} takes
events (@tt{{kind: 'key-pressed', key: 'Space'}}, keys named as C++'s @tt{zgal::Key}), and
@tt{state}, @tt{setState}, @tt{quitRequested()} and @tt{takeResize()} are there for pages and tests.
@racket[print!] writes through @tt{zgal.output.line} (@tt{console.log} unless replaced), formatting
floats as C++'s streams do.

@zgal-example[#:web? #t #:show '("example.zgal.ts") #:from #rx"^  beginFrame" #:to #rx"^  }$"
"(compute-material Fill
  (workgroup 1)
  (bindings (values (buffer-array uint)))
  (push (value uint))
  (compute (write! (at values 0) value)))

(graph Counting
  (input count (per-frame uint))
  (buffer counts uint 1 #:frames shared)
  (material fill Fill (values counts) #:push ((value count)) #:invocations (1))
  (dispatch filling fill)
  (readback readCounts counts))

(app Counter
  (state (count uint 0) (paused bool #f) (seen (data uint) (empty-data uint)))
  (on (key-pressed space) (with state #:paused (not state.paused)))
  (on (frame dt) (if state.paused state (with state #:count (+ state.count 1))))
  (on (read readCounts copied) (with state #:seen copied))
  (run Counting (count state.count) #:enabled ((filling (not state.paused)))))"]

A program's main runs headless under Node, @tt{node FILE.zgal.main.ts --frames N}, with the native
program's options (@secref["main"]) and its own, writing captures (@tt{--capture},
@tt{--capture-frames}) to PNG files and exiting with 1 for errors or WebGPU's validation errors. A
program without a window runs headless in its page too (see below). A program with a window runs in
its page, @tt{FILE.zgal.html}, which loads the main built to
JavaScript (@tt{zgal-web-program}, @secref["building"]): the app draws into the page's canvas
each animation frame, at the window's size scaled to fit the page, with the canvas's keyboard,
mouse and wheel events, and options from the query string (@tt{?frames=60&level=3}, @tt{arg=...}
for positional arguments). Serve the directory over HTTP (modules don't load from @tt{file:} URLs)
and open the page in a browser with WebGPU (e.g. Firefox, Safari or Chrome); headless browsers
usually have no WebGPU adapter, which the page reports. Assets are found in @tt{assets/} beside the main,
as native programs find theirs. The page's state is in its root element's @tt{data-zgal} attribute
(@tt{running}, then @tt{stopped} when the app quits or runs its @tt{frames}, or @tt{failed}), and
@tt{web/tools/firefox.ts} uses it to check a page in Firefox: it serves a directory, opens the page
(in a window, for WebGPU), prints its console, presses keys and compares the canvas with an
approved PNG, e.g. @tt{node web/tools/firefox.ts --serve build/web/smb3 --compare 120.png
'smb3.zgal.html?frames=120'}. @tt{web/tools/chromium.ts} does the same in headless Chromium (by
default Playwright's headless shell) on SwiftShader, its software adapter, needing no window or GPU.
@tt{make test-pages} checks the examples' pages and its test pages in each browser of
@tt{BROWSERS}: by default only headless Chromium, as Firefox opens windows; @tt{make test-pages
BROWSERS="chromium firefox"} adds it. A browser found nowhere is skipped, but one @tt{CHROMIUM} or
@tt{FIREFOX} names must exist. Headless Chromium shows WebGPU canvases of a SwiftShader device only
with all of @tt{--use-angle=swiftshader --enable-features=Vulkan --use-vulkan=swiftshader}, on macOS
and Linux alike, which the tool passes (see @tt{docs/web.md}).

Other pages embed programs: the main of a program with a window exports @tt{mount(canvas, options)},
which runs it in a canvas of the page (@tt{zgal.mountProgram}) and returns an object whose
@tt{stop()} stops it and frees its WebGPU device, its graphs and its listeners (while it starts,
too). The options are @tt{scaling} (required: @tt{'whole-multiples'} shows the canvas at the largest
whole multiple of its size that fits, pixelated, @tt{'fit'} at the largest size, smoothly),
@tt{args} (its command line, as under Node), @tt{keys} (@tt{'canvas'}, the default, takes key
presses while the canvas has focus, which it takes when clicked; @tt{'page'} takes the whole
page's), @tt{onState} (called with @tt{running}, @tt{stopped} or @tt{failed}) and @tt{onMessage}
(errors and usage, by default to the console). The canvas is shown at the centre of its parent,
which must have a size of its own. Each mounted program has its own device, so a page can run
several, e.g.

@verbatim{
import { mount } from './particles/particles.zgal.main.js';
const particles = mount(document.querySelector('canvas'), { scaling: 'fit', args: ['--particles', '20000'] });
// ... later
await particles.stop();
}

Its own page calls the main's @tt{run()}, which mounts the program in the page's canvas, and Node
runs the main when it is the program run (not imported). @tt{make test-pages} checks mounting with
@tt{web/tests/mount.html}.

Every program's main also exports @tt{runHeadless(options)} (@tt{zgal.runHeadless}), which runs it
headless in a page as Node would, with its own WebGPU device: its frames (@tt{--frames}, by default
1 for a program without a window; a program with a window needs it), its captures given to a
callback rather than written. The options are @tt{args} (required: its command line, as under
Node), @tt{files} (the files it reads, by the paths @tt{args} gives them: a @tt{Blob}, e.g. a file
input's @tt{File}, or a URL relative to the page; other paths are fetched from @tt{assets/}),
@tt{onCapture} (called with each capture: its @tt{path}, the file Node would write, @tt{frame}
(@tt{--capture-frames}' frame, or null), @tt{size}, @tt{rgba} pixels and @tt{png}, a PNG
@tt{Blob}), @tt{onState} and @tt{onMessage}, as @tt{mount}'s. It returns an object whose @tt{done}
promise gives the state it ended in (@tt{stopped}, or @tt{failed} for a bad command line, an
error or WebGPU's validation errors) and whose @tt{stop()} stops it, resolving once its device is
destroyed. Runs are independent, each creating a device, so a page running again on input (e.g. a
slider) should @tt{await} the previous run's @tt{stop()} before starting the next, and start only the latest of several quick inputs, e.g.

@verbatim{
import { runHeadless } from './image_filter/image_filter.zgal.main.js';
let run = null, latest = 0;
async function filter(file, blur) {
  const mine = ++latest;
  await run?.stop();
  if (mine !== latest) return;
  run = runHeadless({
    args: [file.name, 'out.png', '--blur', String(blur)],
    files: { [file.name]: file },
    onCapture: (capture) => {
      if (mine !== latest) return;
      URL.revokeObjectURL(image.src);
      image.src = URL.createObjectURL(capture.png);
    },
  });
}
}

The main of a program without a window exports @tt{mount(container, options)} instead of the
canvas's (@tt{zgal.mountHeadless}): it adds a form to the element, with an input per positional
argument, option and flag (a file input for each path the program reads; the path its capture
goes to, @tt{capture #:to}, is named by the page), the number of frames and a Run button, then the
last run's captures, each an image and a link downloading it. The options are @tt{args} (a command
line to run at once), @tt{onRun} (called with each run's command line), @tt{onState} and
@tt{onMessage}. Running again stops the run before (only the last of several quick runs starts, and
a stopped run's states and captures don't follow the next run's @tt{onRun}, and its messages go to
@tt{console.debug}); @tt{stop()} stops the run, reporting its last messages and @tt{stopped}, and removes
the form. Its markup has classes for a page's
styles (@tt{form.zgal-headless}, @tt{label.zgal-field}, @tt{.zgal-captures}), and the program's
@tt{program.options} describe its options (@tt{kind}, @tt{name}, @tt{metavar}, @tt{help}, @tt{value},
@tt{default}, @tt{range}, and @tt{output} for where the capture goes) for a page building its own
controls. The program's page shows the form, running the query string's command line at once when
it gives one (@tt{?arg=photo.jpg&arg=out.png&blur=2}, paths fetched relative to @tt{assets/}), with
the run's state in @tt{data-zgal}. @tt{make test-pages} runs image_filter's page and checks
@tt{web/tests/headless.html}; the checkers' @tt{--save-capture FILE} saves the page's capture.

Read-write storage images other than @tt{r32float}, @tt{r32uint} and @tt{r32sint} need WebGPU's
@tt{texture-formats-tier2} feature, which the runtime requests when the adapter has it; without it,
a material binding one reports which image to bind read or write instead.

Graphs on the web can use the compute library's nodes (@secref["compute-library"]). Where the device has no
subgroups (WebGPU's @tt{subgroups} feature is optional), the runtime runs their materials'
variants scanning, reducing and ranking in shared memory instead, which the compiler generates
beside them. Indirect dispatches and draws run on the web too (@secref["indirect"]), and so do
models and meshes: the runtime loads glTF (@tt{.gltf}, @tt{.glb}) and OBJ files itself, as the
C++ does, and a meshes input is an array of the runtime's @tt{Mesh}es, which @tt{createMesh} makes
from vertices @tt{encodeVertices} lays out as the module's vertex format descriptions (a constant
per vertex format) say. What a program's own code gives the app goes through
@tt{mainInputs} on the web too (@secref["main-inputs"]). WebGPU's samplers have no border colour, and its buffers can't be
mapped while the GPU uses them, so samplers clamping to a border and cpu nodes reading or writing
imported buffers can't be either. Compiling any of them for the web reports
@secref["error-not-on-web"].

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
 @item{in shader bodies: recursion, atomics on images and on 64-bit or small integers, arithmetic
       on 8- and 16-bit integers (they're widened), and functions reading bindings (pass them as
       arguments). GLSL bodies and functions (@racket[extern-glsl]) can do these;}
 @item{several vertex buffers, or per-instance attributes;}
 @item{images other than 2D ones;}
 @item{a shader used by more than one material.}
]

Graphs can't yet use storage images other than 2D ones, run a node on a chosen queue, or compute
anything per frame but push constants, dispatch sizes and uniforms. Loops of nodes are fixed when
the graph is built (a node list that changes from frame to frame needs a rebuilt graph), go over
ranges and data inputs (not lists within their elements).

Apps handle events only: they can't ask whether a key is held or where the cursor is outside
handlers (track it in the state from press and release events), read files, or create resources.

ZGAL has grown in stages: interfaces, graphs, shader bodies (with functions shared by the GLSL and
C++ backends, and bindings' accesses inferred), then apps. The compiler's checked program (@tt{dsl/private/ast.rkt}) describes interfaces
without assuming a backend, so other backends only add emitters. The web backend (@secref["web"])
writes WGSL and TypeScript for materials, graphs, apps and programs, which run in browsers and
under Node; next is what graphs on the web can't use yet.

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
  (list @tt{dsl/private/syntax.rkt} "helpers for reading forms: shapes, names, namespaces")
  (list @tt{dsl/private/graph.rkt} "reading and checking graphs, and their expressions' types")
  (list @tt{dsl/private/builtins.rkt} "shader bodies' built-in functions and stage variables")
  (list @tt{dsl/private/body.rkt} "checking shader bodies and functions into typed expressions, inlining, and inferring accesses")
  (list @tt{dsl/private/lower.rkt} "lowering typed expressions to statements, for any backend")
  (list @tt{dsl/private/print-body.rkt} "printing lowered bodies and functions as GLSL or C++")
  (list @tt{dsl/private/ast.rkt} "the checked program, independent of any backend")
  (list @tt{dsl/private/layout.rkt} "std430 layouts, and which types each place accepts")
  (list @tt{dsl/private/features.rkt} "the device features (8- and 16-bit integers) materials need")
  (list @tt{dsl/private/names.rkt} "valid and reserved names")
  (list @tt{dsl/lib/} "the standard library's modules")
  (list @tt{dsl/lib/kernels/} "the compute library's materials, which graph.rkt instantiates (parse.rkt reads them)")
  (list @tt{dsl/private/errors.rkt} "the error catalog")
  (list @tt{dsl/private/writer.rkt} "writing generated code with #line directives")
  (list @tt{dsl/private/glsl.rkt} "the GLSL backend")
  (list @tt{dsl/private/wgsl.rkt} "the WGSL backend (WebGPU)")
  (list @tt{dsl/private/ts.rkt} "the TypeScript backend (WebGPU): structs and materials")
  (list @tt{dsl/private/ts-graph.rkt} "the TypeScript backend for graphs")
  (list @tt{dsl/private/ts-body.rkt} "printing lowered bodies as TypeScript: functions, constants and apps' bodies")
  (list @tt{dsl/private/ts-app.rkt} "the TypeScript backend for apps")
  (list @tt{dsl/private/ts-main.rkt} "the TypeScript backend for mains, and programs' pages")
  (list @tt{dsl/private/ts-common.rkt} "TypeScript spellings the backend's modules share")
  (list @tt{dsl/private/cpp.rkt} "the C++ backend: structs, vertex formats and materials")
  (list @tt{dsl/private/cpp-graph.rkt} "the C++ backend for graphs")
  (list @tt{dsl/private/app.rkt} "reading and checking apps")
  (list @tt{dsl/private/cpp-app.rkt} "the C++ backend for apps")
  (list @tt{dsl/private/main.rkt} "reading and checking a program's main")
  (list @tt{dsl/private/cpp-main.rkt} "the C++ backend for mains")
  (list @tt{dsl/private/cpp-common.rkt} "C++ spellings the backend's modules share")
  (list @tt{include/zgal/Runtime.h} "what the generated graphs use at runtime: FrameInfo, Uniform, Instance, addCountedDraw")
  (list @tt{include/zgal/Material.h} "what the generated materials share: their checks and shaders")
  (list @tt{include/zgal/App.h} "what the generated apps use: events, Timestep, RunMode")
  (list @tt{include/zgal/Main.h} "what the generated mains use: the command line, validation, captures, running headless")
  (list @tt{include/zgal/Window.h} "running an app in a VulkanApp's window, and windowed programs")
  (list @tt{dsl/docs/} "this document, and the helpers compiling its examples")
  (list @tt{web/zgal/} "what the generated TypeScript uses at runtime: Context, RenderGraph, nodes, materials, types' layouts, apps' events and clocks, C++'s arithmetic (math.ts) and built-in functions (glsl.ts), mains (main.ts, main-node.ts, main-browser.ts), PNGs (png-pixels.ts, png-write.ts); node.ts and png.ts for Node")
  (list @tt{web/} "the web backend's packages (npm: TypeScript, and Dawn, WebGPU for Node), tools (tools/check-wgsl.ts compiles WGSL with Dawn; tools/firefox.ts and tools/chromium.ts check a page in Firefox or headless Chromium) and tests")
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
 @item{@tt{make test-dsl} builds this document and runs the tests, and @tt{make test-web} the web
       runtime's; @tt{make test} includes both.}
]

@subsection{Tests}

@itemlist[
 @item{@tt{dsl/tests/layout-test.rkt}: std430 layouts.}
 @item{@tt{dsl/tests/errors-test.rkt}: the error catalog, and the errors' messages and locations.}
 @item{@tt{dsl/tests/body-test.rkt}: shader bodies: typing, scoping, inference, inlining, which
       functions compile to C++, small integers, shared memory and barriers.}
 @item{@tt{dsl/tests/app-test.rkt}: apps: handlers, events, effects and their graphs.}
 @item{@tt{dsl/tests/module-test.rkt}: imports, the standard library, namespaces, and data in and
       out of graphs (uploads, clears, copies into images, readbacks).}
 @item{@tt{dsl/tests/golden-test.rkt}: the output for @tt{golden/features.zgal}, which uses every
       feature, and for the web for @tt{golden/web.zgal}, what the WGSL backend does differently.}
 @item{@tt{dsl/tests/toolchain-test.rkt}: the generated shaders and header through glslc and the C++
       compiler (with the build's flags, from @tt{make test-dsl}), @tt{#line} mapping, the
       command line, the golden and examples' WGSL modules through Dawn, and the golden
       TypeScript through @tt{tsc} (@tt{web/}, whose packages @tt{make test-dsl} installs with
       @tt{npm ci}).}
 @item{@tt{dsl/tests/web-test.rkt}: what compiling for the web reports that Vulkan allows.}
 @item{@tt{web/tests}: @tt{web.zgal} and @tt{apps.zgal} compiled to TypeScript and WGSL and run on Dawn under Node
       (@tt{make test-web}, which type checks it first): the generated WGSL computes what the GLSL
       would (layouts of small integers, shared memory, emulated atomics and subgroup scans,
       expressions WGSL spells differently, draws with depth tests), and generated graphs run as
       the C++ ones do (loops, conditions, enables and once nodes, per-frame inputs, uploads,
       copies, buffer files, cpu and extern nodes), functions compute what the GPU and C++ do,
       and apps handle events, effects, read events, externs and phases as C++ apps do, without
       WebGPU errors. @tt{make test-web} then builds the particles example for the web and runs it
       under Node.}
 @item{@tt{tests/gpu/ZgalTest.cpp}: generated materials and graphs on the device: the C++ structs
       and push constants have the layout the shaders write and read (8- and 16-bit integers
       included), a function computes the same on the CPU (as C++) as on the GPU, a reduction
       through shared memory, imported buffers, copies, cpu and extern nodes in a graph, and an
       app's state, events, extern functions and graph inputs, and uploads, clears, copies into
       images and buffer readbacks. @tt{tests/gpu/VulkanAppTest.cpp} checks
       @tt{zgal::connect} turns window input into events.}
]
