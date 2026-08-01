;; Adapted from kaappi/tests/scheme/differential/probes/boolean-simplify.scm
;; Bootstrap subset: no reverse (define locally), keep shadowed `not`.

(define seen '())
(define (note! tag v) (set! seen (cons tag seen)) v)
(define (rev xs)
  (define (loop xs acc)
    (if (null? xs) acc (loop (cdr xs) (cons (car xs) acc))))
  (loop xs '()))

(if (not #f) (begin (display "nf-then") (newline)) (begin (display "nf-else") (newline)))
(if (not #t) (begin (display "nt-then") (newline)) (begin (display "nt-else") (newline)))
(if (not (= 1 2)) (begin (display "ne-then") (newline)) (begin (display "ne-else") (newline)))
(if (not (= 1 1)) (begin (display "eq-then") (newline)) (begin (display "eq-else") (newline)))

(if (not #f) (begin (display "onearm-ran") (newline)))
(if (not #t) (begin (display "ONEARM-WRONG") (newline)))
(display (if (not #t) 'yes)) (newline)
(write (if (not #t) 'yes)) (newline)

(if (not (not #t)) (begin (display "dn-then") (newline)) (begin (display "dn-else") (newline)))
(if (not (not (not #t))) (begin (display "tn-then") (newline)) (begin (display "tn-else") (newline)))

(if (not (note! 'a #f)) (begin (display "side-then") (newline)) (begin (display "side-else") (newline)))
(if (not (note! 'b #t)) (begin (display "side2-then") (newline)) (begin (display "side2-else") (newline)))
(display (rev seen)) (newline)

(define (not x) 'always-truthy)
(if (not #f) (begin (display "shadowed-then") (newline)) (begin (display "shadowed-else") (newline)))
(if (not #t) (begin (display "shadowed2-then") (newline)) (begin (display "shadowed2-else") (newline)))
