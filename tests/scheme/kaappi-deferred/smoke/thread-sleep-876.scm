;; Regression test for #876: thread-sleep! must actually sleep

(import (srfi 18) (scheme base))
(define s0 (current-second))
(thread-sleep! 0.1)
(define s1 (current-second))
(display (>= (- s1 s0) 0.09))
(newline)
