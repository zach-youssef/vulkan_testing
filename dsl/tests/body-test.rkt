#lang racket/base
;; Shader bodies written in ZGAL: typing, scoping, inference and what they generate

(require racket/list
         racket/string
         rackunit
         "../compile.rkt"
         "../private/ast.rkt"
         "util.rkt")

;; The program for text, checked
(define (program-of text)
  (define-values (prog _) (compile-zgal "test.zgal" #:text text #:check-only? #t))
  prog)

(define (material-named prog name)
  (for/first ([m (in-list (program-materials prog))] #:when (string=? (material-name m) name)) m))

(define (binding-named m name)
  (for/first ([b (in-list (material-bindings m))] #:when (string=? (binding-name b) name)) b))

;; The generated output named file
(define (generated text name)
  (output-text (for/first ([o (in-list (compile-text text))] #:when (string-suffix? (output-path o) name)) o)))

(define particles
  (string-append
   "(struct Particle (position vec2) (velocity vec2))\n"
   "(graphics-material Draw\n"
   "  (bindings (particles (buffer-array Particle)) (tint (buffer vec4)) (albedo texture) (albedoSampler sampler))\n"
   "  (varyings (uv vec2))\n"
   "  (outputs (color vec4))\n"
   "  (vertex (results #:position (vec4 (.position (at particles vertexIndex)) 0.0 1.0) #:uv (vec2 0.5)))\n"
   "  (fragment (* tint (sample albedo albedoSampler uv))))\n"
   "(compute-material Step\n"
   "  (workgroup 64)\n"
   "  (bindings (particles (buffer-array Particle)))\n"
   "  (compute\n"
   "   (let* ([i globalInvocationId.x] [p (at particles i)])\n"
   "     (write! (at particles i) (with p #:position (+ p.position p.velocity))))))\n"))

(test-case "Accesses and stages are inferred from ZGAL bodies"
  (define prog (program-of particles))
  (define draw (material-named prog "Draw"))
  (check-equal? (binding-access (binding-named draw "particles")) 'read)
  (check-equal? (binding-stages (binding-named draw "particles")) '(vertex))
  (check-equal? (binding-stages (binding-named draw "tint")) '(fragment))
  (check-equal? (binding-stages (binding-named draw "albedo")) '(fragment))
  (check-equal? (binding-access (binding-named (material-named prog "Step") "particles")) 'read-write))

(test-case "A binding no ZGAL body uses needs its access declared"
  (check-zgal-error (string-append particles "(compute-material Unused (workgroup 1) (bindings (steps (buffer uint))) (compute))")
                    'unused-binding "binding steps isn't used"))

(test-case "Declared accesses and stages must match the bodies"
  (check-zgal-error (string-append "(graphics-material Flat (bindings (tint (buffer vec4) #:stages (vertex))) (outputs (color vec4))\n"
                                   "  (vertex (results #:position (vec4 0.0))) (fragment tint))")
                    'access-mismatch "declared for the vertex shader, but the fragment shader uses it"))

(test-case "Locals can't hide outputs, functions or bindings, and outputs can't be read"
  (define (graphics vertex fragment)
    (string-append "(define (halve [x : float]) : float (* x 0.5))\n"
                   "(graphics-material Flat (bindings (tint (buffer vec4))) (varyings (uv vec2)) (outputs (color vec4))\n"
                   "  (vertex " vertex ")\n  (fragment " fragment "))"))
  (define ok-vertex "(results #:position (vec4 0.0) #:uv (vec2 0.0))")
  (check-zgal-error (graphics "(let* ([uv (vec2 1.0)]) (results #:position (vec4 0.0) #:uv uv))" "tint")
                    'duplicate-name "uv is already declared outside this body")
  (check-zgal-error (graphics ok-vertex "(let* ([halve 1.0]) tint)") 'duplicate-name "halve")
  (check-zgal-error (graphics ok-vertex "(let* ([tint 1.0]) (vec4 tint))") 'duplicate-name "tint")
  (check-zgal-error (graphics ok-vertex "color") 'unknown-name "color is an output of the fragment shader"))

(test-case "Stage variables belong to their stage, and functions take them as arguments"
  (check-zgal-error "(compute-material C (workgroup 1) (compute (let* ([i vertexIndex]) (begin))))"
                    'unknown-name "vertexIndex is a built-in variable of another stage")
  (check-zgal-error "(define (f) : uint globalInvocationId.x)" 'unknown-name "functions can't use stage variables"))

(test-case "Expressions are typed like GLSL, with integer literals adapting"
  (define (fn body [result "float"]) (format "(define (f [a : float] [v : vec3] [i : int] [u : uint]) : ~a ~a)" result body))
  (check-false (zgal-error-of (fn "(* 2 a)")))
  (check-false (zgal-error-of (fn "(* v 2)" "vec3")))
  (check-false (zgal-error-of (fn "(bitand u 3)" "uint")))
  (check-false (zgal-error-of (fn "(clamp v 0.0 1.0)" "vec3")))
  (check-false (zgal-error-of (fn "(mix v (vec3 1.0) (greaterThan v (vec3 0.5)))" "vec3")))
  (check-zgal-error (fn "(+ a i)") 'type-mismatch "+ of a float and an int isn't defined")
  (check-zgal-error (fn "(* v (vec2 1.0))" "vec3") 'type-mismatch "* of a vec3 and a vec2 isn't defined")
  (check-zgal-error (fn "(dot v a)") 'type-mismatch "dot isn't defined for a vec3, a float")
  (check-zgal-error (fn "v.xyzw" "vec4") 'unknown-name "xyzw isn't a swizzle of vec3")
  (check-zgal-error (fn "(vec3 a a)" "vec3") 'type-mismatch "vec3 needs 3 components")
  (check-zgal-error (fn "(bitand i u)" "int") 'type-mismatch "bitand of an int and a uint"))

(test-case "Folds bind several values, and sum adds"
  (check-false (zgal-error-of
                (string-append "(define (stats [n : int]) : float\n"
                               "  (let* ([(total count) (fold ([total 0.0] [count 0]) ([i (range 0 n)]) (values (+ total (float i)) (+ count 1)))])\n"
                               "    (+ total (float count) (sum ([i (range 0 n)] [j (range 0 i)]) 1.0))))")))
  (check-zgal-error "(define (f [n : int]) : float (fold ([a 0.0] [b 0.0]) ([i (range 0 n)]) a))"
                    'malformed "the loop has 2 accumulators, so its body is (values ...)"))

(test-case "write! writes buffers, and results are checked"
  (check-zgal-error "(compute-material C (workgroup 1) (bindings (result (buffer float))) (compute (let* ([x 1.0]) (write! x 2.0))))"
                    'bad-write "write! writes to a buffer binding")
  (check-zgal-error (string-append "(graphics-material Flat (varyings (uv vec2)) (outputs (color vec4))\n"
                                   "  (vertex (results #:position (vec4 0.0) #:uv (vec2 0.0) #:extra 1.0)) (fragment (vec4 1.0)))")
                    'bad-results "has no output extra")
  (check-zgal-error (string-append "(graphics-material Flat (varyings (uv vec2)) (outputs (color vec4))\n"
                                   "  (vertex (results #:position (vec4 0.0) #:uv (vec2 0.0))) (fragment (vec3 1.0)))")
                    'type-mismatch "output color is a vec4, not a vec3"))

(test-case "Constants are computed from literals, constants, constructors, operators and built-ins"
  (check-false (zgal-error-of "(define tau (* 2.0 3.14159))\n(define axis (normalize (vec3 1 tau 0)))"))
  (check-zgal-error "(define (f) : float 1.0)\n(define x (f))" 'malformed "a constant is computed from"))

(test-case "Functions can only call functions declared before them, so none recurses"
  (check-zgal-error "(define (f [x : float]) : float (f x))" 'unknown-name "f isn't a function"))

(test-case "Pure functions and constants also compile to C++; those using textures, images or effects don't"
  (define prog (program-of (string-append
                            "(define scale 2.0)\n"
                            "(define (twice [x : vec3]) : vec3 (* x scale))\n"
                            "(define (look [t : texture2D] [s : sampler]) : vec4 (sample t s (vec2 0.5)))\n"
                            "(define (load [i : image2D]) : vec4 (image-load i (ivec2 0)))\n")))
  (define cpu (for/hash ([d (in-list (program-functions prog))])
                (if (function-decl? d)
                    (values (function-decl-name d) (function-decl-cpu? d))
                    (values (constant-decl-name d) (constant-decl-cpu? d)))))
  (check-equal? cpu (hash "scale" #t "twice" #t "look" #f "load" #f)))

(test-case "Functions taking storage images are inlined; others are called"
  (define text (string-append
                "(define (load [image : image2D] [texel : ivec2]) : vec4 (let* ([t (clamp texel (ivec2 0) (ivec2 7))]) (image-load image t)))\n"
                "(define (twice [x : vec4]) : vec4 (* x 2.0))\n"
                "(compute-material C (workgroup 1) (bindings (img (storage-image)))\n"
                "  (compute (let* ([t (ivec2 globalInvocationId.xy)]) (store! img t (twice (load img t))))))"))
  (define shader (generated text "C.comp"))
  (check-false (regexp-match? #rx"vec4 load[(]" shader) "load isn't emitted")
  (check-regexp-match #rx"vec4 twice[(]vec4 x[)]" shader)
  ;; The inlined body's local is respelled, so it doesn't clash with the caller's t
  (check-regexp-match #rx"ivec2 t_[0-9]+ = clamp" shader)
  (check-equal? (binding-access (binding-named (material-named (program-of text) "C") "img")) 'read-write))

(test-case "Effectful branches and folds lower to statements"
  (define text (string-append
                "(compute-material C (workgroup 1) (bindings (result (buffer-array float)))\n"
                "  (compute (let* ([i globalInvocationId.x])\n"
                "    (when (< i 4) (write! (at result i) (if (= i 0) (sum ([k (range 0 4)]) (float k)) 1.0))))))"))
  (define shader (generated text "C.comp"))
  (check-regexp-match #rx"if [(]i < 4u[)] {" shader)
  (check-regexp-match #rx"for [(]int k = 0; k < 4; [+][+]k[)] {" shader)
  (check-regexp-match #rx"zgalBranch" shader)
  ;; A branch choosing a fold's next values assigns a temporary each
  (define chosen (generated (string-append
                             "(compute-material Pick (workgroup 1) (bindings (result (buffer-array uint)))\n"
                             "  (compute (let* ([(best where) (fold ([best (uint 0)] [where (uint 0)]) ([i (range 0 4)])\n"
                             "                                   (if (> (at result i) best) (values (at result i) (uint i)) (values best where)))])\n"
                             "    (write! (at result 0) (+ best where)))))")
                            "Pick.comp"))
  (check-regexp-match #rx"uint zgalBranch[0-9]+;\n[^\n]*\n *uint zgalBranch[0-9]+;" chosen)
  (check-regexp-match #rx"zgalBranch[0-9]+ = uint[(]i[)];" chosen))

;; ---------------------------------------------------------------------------------------------
;; 8- and 16-bit integers

(define (bytes-material body)
  (string-append "(struct Bytes (small uint8) (delta int16) (many (array uint8 4)))\n"
                 "(compute-material Pack (workgroup 1) (bindings (data (buffer-array Bytes)))\n"
                 "  (compute " body "))"))

(test-case "Small integers are widened where they're read, and narrowed explicitly where they're written"
  (define shader
    (generated (bytes-material "(let* ([b (at data 0)]) (write! (at data 1) (with b #:small (uint8 (+ b.small 1)) #:delta (int16 (- b.delta)))))")
               "Pack.comp"))
  (check-true (string-contains? shader "uint8_t(uint(b.small) + 1u)"))
  (check-true (string-contains? shader "int16_t(-int(b.delta))"))
  (check-true (string-contains? shader "#extension GL_EXT_shader_explicit_arithmetic_types_int8 : require"))
  ;; Writing a 32-bit value to a small place needs a conversion
  (check-zgal-error (bytes-material "(write! (.small (at data 0)) (uint 1))") 'type-mismatch "is a uint8, not a uint")
  ;; Literals take the small type, within its range
  (check-not-exn (λ () (program-of (bytes-material "(write! (.small (at data 0)) 255)"))))
  (check-zgal-error (bytes-material "(write! (.small (at data 0)) 256)") 'type-mismatch "256 doesn't fit in a uint8 (0 to 255)")
  (check-zgal-error (bytes-material "(write! (.delta (at data 0)) (uint16 -1))") 'type-mismatch "-1 doesn't fit in a uint16"))

(test-case "Small integers aren't push constants, and a material using them needs device features"
  (check-zgal-error "(compute-material P (workgroup 1) (push (x uint8)) (compute))" 'type-not-allowed "storagePushConstant8")
  (define header (generated (bytes-material "(write! (.small (at data 0)) 1)") ".zgal.h"))
  (check-true (string-contains? header "features.vulkan12.shaderInt8 = VK_TRUE;"))
  (check-true (string-contains? header "features.core.features.shaderInt16 = VK_TRUE;"))
  ;; Checked before loading the shaders, which use them
  (check-true (string-contains? header "loadShaders(VulkanContext& context) {\n        zgal::requireFeatures(context, requiredFeatures(), ")))

;; ---------------------------------------------------------------------------------------------
;; Shared memory and barriers

(define (shared-material body)
  (string-append "(compute-material Sum (workgroup 64) (shared (partial (array float 64)))\n"
                 "  (bindings (result (buffer float)) (samples (buffer-array float))) (push (stride uint))\n"
                 "  (compute (let* ([x localInvocationId.x]) (write! (at partial x) (at samples x)) " body ")))"))

(test-case "Compute shaders write shared variables, and reach barriers together"
  (define shader (generated (shared-material "(write! (at partial x) (at samples x)) (barrier!) (when (= x 0) (write! result (at partial 63)))")
                            "Sum.comp"))
  (check-true (string-contains? shader "shared float partial[64];"))
  (check-true (string-contains? shader "partial[x] = samples()[x];"))
  (check-true (string-contains? shader "barrier();"))
  ;; Shared variables aren't bindings: the accesses are the buffers'
  (define m (material-named (program-of (shared-material "(write! (at partial x) (at samples x)) (write! result 1.0)")) "Sum"))
  (check-equal? (binding-access (binding-named m "samples")) 'read))

(test-case "Barriers under conditions or loops that can differ between invocations are rejected"
  ;; Uniform: literals, push constants, workgroupId, and what's computed from them
  (check-not-exn (λ () (program-of (shared-material "(for ([i (range 0 stride)]) (barrier!)) (when (> workgroupId.x 2) (barrier!)) (write! result 0.0)"))))
  (check-zgal-error (shared-material "(when (= x 0) (barrier!)) (write! result 0.0)") 'bad-barrier "under a condition")
  (check-zgal-error (shared-material "(for ([i (range 0 x)]) (barrier!)) (write! result 0.0)") 'bad-barrier "in a loop")
  ;; A value read from a buffer can differ
  (check-zgal-error (shared-material "(let* ([n (at samples x)]) (when (> n 0.0) (barrier!))) (write! result 0.0)") 'bad-barrier "under a condition")
  (check-zgal-error "(graphics-material F (outputs (color vec4)) (vertex (results #:position (vec4 0.0))) (fragment (begin (barrier!) (vec4 1.0))))"
                    'bad-barrier "only compute shaders"))

;; ---------------------------------------------------------------------------------------------
;; Atomics, loops, early exits, subgroups and derivatives

(define (counting-material body)
  (string-append "(compute-material Count (workgroup 64) (shared (bins (array uint 16)))\n"
                 "  (bindings (counts (buffer-array uint)) (sums (buffer-array float)) (samples (buffer-array uint)))\n"
                 "  (compute (let* ([i globalInvocationId.x]) " body ")))"))

(test-case "Atomics give the previous value, which sequences may ignore, and read and write their places"
  (define text (counting-material "(atomic-add! (at bins (remainder i 16)) 1)\n(let* ([old (atomic-max! (at counts 0) (at samples i))]) (write! (at counts 1) old)) (atomic-add! (at sums 0) 1.0)"))
  (define shader (generated text "Count.comp"))
  (check-true (string-contains? shader "uint zgalAtomic1 = atomicAdd(bins[i % 16u], 1u);"))
  (check-true (string-contains? shader "uint zgalAtomic2 = atomicMax(counts()[0], samples()[i]);"))
  (check-true (string-contains? shader "#extension GL_EXT_shader_atomic_float : require"))
  (define m (material-named (program-of text) "Count"))
  (check-equal? (binding-access (binding-named m "counts")) 'read-write)
  (check-equal? (binding-access (binding-named m "sums")) 'read-write)
  ;; Float adds need a device feature
  (check-true (string-contains? (generated text ".zgal.h") "features.atomicFloat.shaderBufferFloat32AtomicAdd = VK_TRUE;"))
  (check-zgal-error (counting-material "(atomic-min! (at sums 0) 1.0) (write! (at counts 0) 0)") 'type-mismatch "floats have atomic-add!")
  (check-zgal-error (counting-material "(let* ([x 1]) (atomic-add! x 1)) (write! (at counts 0) (at samples 0)) (write! (at sums 0) 0.0)")
                    'bad-write "isn't one"))

(test-case "Strided ranges and while loops"
  (define shader (generated (counting-material "(write! (at counts i) (sum ([k (range i 1024 64)]) (at samples k))) (let* ([(n steps) (while ([n (at samples i)] [steps 0]) (> n 1) (values (if (= (remainder n 2) 0) (/ n 2) (+ (* 3 n) 1)) (+ steps 1)))]) (write! (at sums i) (float steps)))")
                            "Count.comp"))
  (check-true (string-contains? shader "for (uint k = i; k < 1024u; k += 64u) {"))
  (check-true (string-contains? shader "while (n > 1u) {"))
  (check-zgal-error (counting-material "(write! (at counts 0) (sum ([k (range 0 4 0)]) k)) (write! (at sums 0) (at sums 1)) (write! (at samples 0) 0)")
                    'malformed "a range's step is positive"))

(test-case "Shaders return and discard early, and barriers after a return only some invocations take are rejected"
  (check-true (string-contains? (generated (counting-material "(unless (< i 100) (return!)) (write! (at counts i) 1) (write! (at sums i) 1.0) (write! (at samples i) 1)") "Count.comp")
                                "        return;"))
  (check-zgal-error (counting-material "(when (> i 100) (return!)) (barrier!) (write! (at counts i) 1) (write! (at sums i) 1.0) (write! (at samples i) 1)")
                    'bad-barrier "after a return! only some invocations take")
  ;; A return every invocation takes (or none) is fine
  (check-not-exn (λ () (program-of (counting-material "(when (> workgroupId.x 100) (return!)) (barrier!) (write! (at counts i) 1) (write! (at sums i) 1.0) (write! (at samples i) 1)"))))
  (check-zgal-error "(graphics-material F (outputs (color vec4)) (vertex (results #:position (vec4 0.0))) (fragment (return!) (vec4 1.0)))"
                    'wrong-stage "only compute shaders")
  (check-true (string-contains? (generated "(graphics-material F (varyings (uv vec2)) (outputs (color vec4)) (vertex (results #:position (vec4 0.0) #:uv (vec2 0.0))) (fragment (when (< (fwidth uv.x) 0.5) (discard!)) (vec4 1.0)))" "F.frag")
                                "discard;")))

(test-case "Subgroup operations need the device's support in their stages, and derivatives fragment shaders"
  (define text (counting-material "(let* ([total (subgroupAdd (at samples i))]) (when (subgroupElect) (write! (at counts (/ i subgroupSize)) total)) (write! (at sums i) (float (subgroupBallotBitCount (subgroupBallot (> total 0))))))"))
  (define header (generated text ".zgal.h"))
  (check-true (string-contains? header "kSubgroupStages = VK_SHADER_STAGE_COMPUTE_BIT;"))
  (check-true (string-contains? header "kSubgroupOperations = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT;"))
  (check-true (string-contains? (generated text "Count.comp") "#extension GL_KHR_shader_subgroup_arithmetic : require"))
  (check-zgal-error (counting-material "(write! (at sums i) (dFdx 1.0)) (write! (at counts i) 0) (write! (at samples i) 0)") 'wrong-stage "only fragment shaders")
  ;; Functions using them are for shaders only
  (check-false (string-contains? (generated "(define (total [x : uint]) : uint (subgroupAdd x))" ".zgal.h") "total")))

(test-case "Buffer barriers make buffer writes visible to the workgroup, under the same rules as barriers"
  (define shader (generated (counting-material "(write! (at counts i) 1) (buffer-barrier!) (write! (at samples i) (at counts (bitxor i 1))) (write! (at sums i) 0.0)") "Count.comp"))
  (check-true (string-contains? shader "    memoryBarrierBuffer();\n#line 3 \"test.zgal\"\n    barrier();"))
  (check-zgal-error (counting-material "(when (> i 3) (buffer-barrier!)) (write! (at counts i) 1) (write! (at samples i) 1) (write! (at sums i) 0.0)")
                    'bad-barrier "under a condition"))

(test-case "Conversions between kinds keep the shape and change the kind"
  (check-not-exn
   (λ () (compile-text "(compute-material C (workgroup 1) (bindings (b (buffer-array float)) (v (buffer-array vec2)))\n  (compute (write! (at b 0) (+ 1.0 (uintBitsToFloat (uint 5)))) (write! (at v 0) (* 2.0 (intBitsToFloat (ivec2 1 2))))))"))))
