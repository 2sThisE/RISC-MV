.section .text
.weak optional_hook
.type optional_hook, function

optional_hook:
    RET
.size optional_hook, $ - optional_hook
