;; Adapted from kaappi/tests/scheme/differential/probes/dead-branch.scm
;; Bootstrap subset: no character literals requiring #\null, no error arms.

(if #t (begin (display "t-then") (newline)) (begin (display "t-else") (newline)))
(if #f (begin (display "f-then") (newline)) (begin (display "f-else") (newline)))
(if #t (begin (display "no-alt") (newline)))
(if #f (begin (display "unreachable") (newline)))

(if 0 (begin (display "zero-true") (newline)) (begin (display "zero-FALSE") (newline)))
(if 0.0 (begin (display "flzero-true") (newline)) (begin (display "flzero-FALSE") (newline)))
(if "" (begin (display "emptystr-true") (newline)) (begin (display "emptystr-FALSE") (newline)))
(if '() (begin (display "nil-true") (newline)) (begin (display "nil-FALSE") (newline)))

(if (= 1 1) (begin (display "eq-then") (newline)) (begin (display "eq-else") (newline)))
(if (< 2 1) (begin (display "lt-then") (newline)) (begin (display "lt-else") (newline)))

(if #t (begin (display "live-arm") (newline)) (begin (display "DEAD-ARM-RAN") (newline)))
(if #f (begin (display "DEAD-ARM-RAN") (newline)) (begin (display "live-arm2") (newline)))

(if #t
    (if #f
        (begin (display "NESTED-WRONG") (newline))
        (if #t (begin (display "nested-ok") (newline)) (begin (display "NESTED-WRONG") (newline))))
    (begin (display "NESTED-WRONG") (newline)))

(display (if #f #f)) (newline)
(write (if #f #f)) (newline)
