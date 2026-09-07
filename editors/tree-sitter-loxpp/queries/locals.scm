; Scopes
; A `for_statement` scope covers its header and body: a C-style or for-in loop
; variable is visible in the body and nowhere else, which is what Lox++ does.
(source_file) @local.scope
(function_declaration) @local.scope
(method_definition) @local.scope
(block) @local.scope
(block_arm_body) @local.scope
(for_statement) @local.scope
(match_arm) @local.scope

; Definitions
(variable_declaration name: (identifier) @local.definition.var)
(object_pattern (identifier) @local.definition.var)
(list_pattern (identifier) @local.definition.var)

(parameters (identifier) @local.definition.parameter)

(function_declaration name: (identifier) @local.definition.function)
(method_definition name: (identifier) @local.definition.method)

(class_declaration name: (identifier) @local.definition.type)
(enum_declaration name: (identifier) @local.definition.type)
(enum_variant name: (identifier) @local.definition.constructor)

; for-in loop variable
(for_statement name: (identifier) @local.definition.var)

; match bindings
(binding_pattern name: (identifier) @local.definition.var)
(constructor_pattern binding: (identifier) @local.definition.var)
(record_pattern binding: (identifier) @local.definition.var)
(rest_element name: (identifier) @local.definition.var)
(sequence_pattern (identifier_pattern) @local.definition.var)
(identifier_pattern) @local.definition.var

; References
(identifier) @local.reference
