#lang racket/base
;; Which names a program may declare. Names become GLSL and C++ identifiers, and binding accessors
;; are function-like GLSL macros, so a name mustn't be a keyword or built-in of either language, a
;; name the wrapper's C++ uses, or one the generated code uses.

(require racket/list
         racket/string)

(provide valid-identifier?
         reserved-name-reason
         form-name-reason
         library-names-allowed?)

;; Whether names starting with zgal may be declared: true while reading the compiler's own library
;; code (the kernels graphs instantiate), whose names mustn't clash with a program's
(define library-names-allowed? (make-parameter #f))

(define (valid-identifier? s)
  (and (regexp-match? #px"^[A-Za-z][A-Za-z0-9_]*$" s)
       (not (string-contains? s "__"))))

(define glsl-keywords
  (append
   '("const" "uniform" "buffer" "shared" "attribute" "varying" "coherent" "volatile" "restrict"
     "readonly" "writeonly" "atomic_uint" "layout" "centroid" "flat" "smooth" "noperspective"
     "patch" "sample" "invariant" "precise" "break" "continue" "do" "for" "while" "switch" "case"
     "default" "if" "else" "subroutine" "in" "out" "inout" "int" "void" "bool" "true" "false"
     "float" "double" "discard" "return" "uint" "lowp" "mediump" "highp" "precision" "struct"
     "sampler" "samplerShadow" "subpassInput" "isubpassInput" "usubpassInput" "subpassInputMS"
     ;; Reserved for future use
     "common" "partition" "active" "asm" "class" "union" "enum" "typedef" "template" "this"
     "resource" "goto" "inline" "noinline" "public" "static" "extern" "external" "interface"
     "long" "short" "half" "fixed" "unsigned" "superp" "input" "output" "filter" "sizeof" "cast"
     "namespace" "using")
   ;; Vector and matrix types
   (for*/list ([prefix '("" "i" "u" "b" "d")] [n '(2 3 4)]) (format "~avec~a" prefix n))
   (for*/list ([prefix '("" "d")] [c '(2 3 4)] [r '("" "x2" "x3" "x4")])
     (format "~amat~a~a" prefix c r))
   ;; Opaque types
   (for*/list ([prefix '("" "i" "u")]
               [base '("sampler" "texture" "image")]
               [dims '("1D" "2D" "3D" "Cube" "2DRect" "1DArray" "2DArray" "CubeArray" "Buffer" "2DMS"
                       "2DMSArray")])
     (string-append prefix base dims))
   (for/list ([dims '("1D" "2D" "2DRect" "Cube" "1DArray" "2DArray" "CubeArray")])
     (string-append "sampler" dims "Shadow"))))

(define glsl-builtins
  '("radians" "degrees" "sin" "cos" "tan" "asin" "acos" "atan" "sinh" "cosh" "tanh" "asinh" "acosh"
    "atanh" "pow" "exp" "log" "exp2" "log2" "sqrt" "inversesqrt" "abs" "sign" "floor" "trunc" "round"
    "roundEven" "ceil" "fract" "mod" "modf" "min" "max" "clamp" "mix" "step" "smoothstep" "isnan"
    "isinf" "floatBitsToInt" "floatBitsToUint" "intBitsToFloat" "uintBitsToFloat" "fma" "frexp"
    "ldexp" "packUnorm2x16" "packSnorm2x16" "packUnorm4x8" "packSnorm4x8" "unpackUnorm2x16"
    "unpackSnorm2x16" "unpackUnorm4x8" "unpackSnorm4x8" "packHalf2x16" "unpackHalf2x16"
    "packDouble2x32" "unpackDouble2x32" "length" "distance" "dot" "cross" "normalize"
    "faceforward" "reflect" "refract" "matrixCompMult" "outerProduct" "transpose" "determinant"
    "inverse" "lessThan" "lessThanEqual" "greaterThan" "greaterThanEqual" "equal" "notEqual" "any"
    "all" "not" "uaddCarry" "usubBorrow" "umulExtended" "imulExtended" "bitfieldExtract"
    "bitfieldInsert" "bitfieldReverse" "bitCount" "findLSB" "findMSB" "textureSize"
    "textureQueryLod" "textureQueryLevels" "textureSamples" "texture" "textureProj" "textureLod"
    "textureOffset" "texelFetch" "texelFetchOffset" "textureProjOffset" "textureLodOffset"
    "textureProjLod" "textureProjLodOffset" "textureGrad" "textureGradOffset" "textureProjGrad"
    "textureProjGradOffset" "textureGather" "textureGatherOffset" "textureGatherOffsets"
    "imageSize" "imageSamples" "imageLoad" "imageStore" "imageAtomicAdd" "imageAtomicMin"
    "imageAtomicMax" "imageAtomicAnd" "imageAtomicOr" "imageAtomicXor" "imageAtomicExchange"
    "imageAtomicCompSwap" "atomicAdd" "atomicMin" "atomicMax" "atomicAnd" "atomicOr" "atomicXor"
    "atomicExchange" "atomicCompSwap" "dFdx" "dFdy" "dFdxFine" "dFdyFine" "dFdxCoarse" "dFdyCoarse"
    "fwidth" "fwidthFine" "fwidthCoarse" "interpolateAtCentroid" "interpolateAtSample"
    "interpolateAtOffset" "barrier" "memoryBarrier" "memoryBarrierAtomicCounter"
    "memoryBarrierBuffer" "memoryBarrierShared" "memoryBarrierImage" "groupMemoryBarrier"
    "subpassLoad" "nonuniformEXT" "EmitVertex" "EndPrimitive"))

(define cpp-keywords
  '("alignas" "alignof" "and" "and_eq" "asm" "auto" "bitand" "bitor" "bool" "break" "case" "catch"
    "char" "char8_t" "char16_t" "char32_t" "class" "compl" "concept" "const" "consteval"
    "constexpr" "constinit" "const_cast" "continue" "co_await" "co_return" "co_yield" "decltype"
    "default" "delete" "do" "double" "dynamic_cast" "else" "enum" "explicit" "export" "extern"
    "false" "float" "for" "friend" "goto" "if" "inline" "int" "long" "mutable" "namespace" "new"
    "noexcept" "not" "not_eq" "nullptr" "operator" "or" "or_eq" "private" "protected" "public"
    "register" "reinterpret_cast" "requires" "return" "short" "signed" "sizeof" "static"
    "static_assert" "static_cast" "struct" "switch" "template" "this" "thread_local" "throw" "true"
    "try" "typedef" "typeid" "typename" "union" "unsigned" "using" "virtual" "void" "volatile"
    "wchar_t" "while" "xor" "xor_eq"))

;; Global names the generated header's includes declare (the wrapper's types, std and glm)
(define cpp-wrapper-names
  '("std" "glm" "Material" "GraphicsMaterial" "ComputeMaterial" "MaterialBinding" "Shader"
    "ShaderLibrary" "VertexLayout" "DescriptorEntry" "DescriptorHeap" "DescriptorType"
    "FrameBinding" "PerFrame" "PerFrameHandles" "Handle" "Result" "Error" "ErrorCode"
    "VulkanContext" "Access" "AttachmentFormats" "GraphicsPipelineOptions" "BlendMode"
    "DepthBias" "Buffer" "Image" "Mesh" "Renderable" "MeshRenderable" "RenderGraph" "Node"
    "ResourceManager" "ContractViolation" "contractViolation"))

;; Names the generated code itself declares
(define generated-names
  '("main" "materialPush" "Bindings" "Push" "Shaders" "loadShaders" "vertexLayout" "writePush"))

(define keyword-set (for/hash ([n (in-list (append glsl-keywords cpp-keywords))]) (values n #t)))
(define builtin-set (for/hash ([n (in-list glsl-builtins)]) (values n #t)))
(define wrapper-set (for/hash ([n (in-list cpp-wrapper-names)]) (values n #t)))
(define generated-set (for/hash ([n (in-list generated-names)]) (values n #t)))

;; Why a (valid) identifier can't be declared, or #f if it can
(define (reserved-name-reason s)
  (cond
    [(hash-ref keyword-set s #f) "is a GLSL or C++ keyword or type"]
    [(hash-ref builtin-set s #f) "is a GLSL built-in function"]
    [(hash-ref wrapper-set s #f) "is a name the wrapper's C++ declares"]
    [(hash-ref generated-set s #f) "is a name the generated code declares"]
    [(string-prefix? s "gl_") "starts with gl_, which GLSL reserves"]
    [(and (regexp-match? #px"^(?i:zgal)" s) (not (library-names-allowed?))) "starts with zgal, which the generated code uses"]
    [else #f]))

;; Bodies' forms named like identifiers. A call (count d) is the form, so a function or struct
;; with one of these names could never be called; values (locals, fields, constants) can have them.
(define body-form-names
  '("array" "at" "begin" "collect" "cond" "count" "fetch" "fold" "range" "remainder" "results" "shl" "shr"
    "sum" "unless" "values" "when" "with"))
(define body-form-set (for/hash ([n (in-list body-form-names)]) (values n #t)))

;; Why a function's or struct's name can't be s, or #f if it can be
(define (form-name-reason s)
  (and (hash-ref body-form-set s #f) "is a form of ZGAL's bodies, so calls to it would mean the form"))
