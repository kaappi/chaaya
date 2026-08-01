;; Adapted from kaappi/tests/scheme/differential/probes/redefined-builtins.scm
;; Bootstrap subset: no zero?.

(define (add1-ish a b) (list 'user a b))
(define + add1-ish)
(display (+ 1 2)) (newline)

(define (times-ish a b) (list 'user* a b))
(define * times-ish)
(display (* 6 7)) (newline)
(display (* 6 1)) (newline)
(display (* 6 0)) (newline)

(define (lt-ish a b) 'user<)
(define < lt-ish)
(display (< 1 2)) (newline)
(if (< 1 2) (begin (display "lt-then") (newline)) (begin (display "lt-else") (newline)))

(define (not-ish x) 'user-not)
(define not not-ish)
(display (not #f)) (newline)
(if (not #f) (begin (display "n-then") (newline)) (begin (display "n-else") (newline)))
(if (not #t) (begin (display "n2-then") (newline)) (begin (display "n2-else") (newline)))

(display (- 10 4)) (newline)
(define (sub-ish a b) 'user-)
(define - sub-ish)
(display (- 10 4)) (newline)
