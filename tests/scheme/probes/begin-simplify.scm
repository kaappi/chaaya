;; Adapted from kaappi/tests/scheme/differential/probes/begin-simplify.scm
;; Bootstrap subset: no error-raising mid-begin (car of '()).

(begin (display "a") (begin (display "b") (display "c")) (display "d")) (newline)
(begin (begin (begin (display "deep")))) (newline)
(begin (display 1) (display 2) (display 3) (display 4) (display 5)) (newline)

(display (begin 1 2 3)) (newline)
(display (begin (display "[eff]") 42)) (newline)
(display (begin 'only)) (newline)
(write (begin "str")) (newline)

(display (begin (+ 1 1) (* 2 2) (- 9 3))) (newline)

(begin
  (if #t (display "bi-then") (display "bi-else"))
  (newline)
  (begin
    (if #f (display "bi2-then") (display "bi2-else"))
    (newline)))
