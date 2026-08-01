;; Adapted from kaappi/tests/scheme/differential/probes/identity-elim.scm
;; Bootstrap subset: exact-integer identities and inexact cases Chaaya supports.

(display (+ 7 0)) (newline)
(display (+ 0 7)) (newline)
(display (* 7 1)) (newline)
(display (* 1 7)) (newline)
(display (* 7 0)) (newline)
(display (* 0 7)) (newline)
(display (- 7 0)) (newline)

(display (+ 1.5 0)) (newline)
(display (* 1.5 1)) (newline)
(display (* 1.5 0)) (newline)
(display (* 0 1.5)) (newline)

(define (+ a b) 'user-plus)
(define (* a b) 'user-times)
(display (+ 7 0)) (newline)
(display (* 7 1)) (newline)
(display (* 7 0)) (newline)
