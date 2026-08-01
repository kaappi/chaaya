;; Adapted from kaappi/tests/scheme/differential/probes/const-fold.scm
;; Bootstrap subset: fixnum/boolean folds only (no zero?/even?/expt/exact?/bignum).

(display (+ 1 2)) (newline)
(display (- 10 4)) (newline)
(display (* 6 7)) (newline)
(display (- 5)) (newline)

(display (< 1 2)) (newline)
(display (> 1 2)) (newline)
(display (= 3 3)) (newline)
(display (<= 3 3)) (newline)
(display (>= 2 3)) (newline)

(display (not #f)) (newline)
(display (not #t)) (newline)

(display (+ 1 2.0)) (newline)
