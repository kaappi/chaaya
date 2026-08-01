(import (scheme base) (scheme process-context) (srfi 64))
(define %test-fail-count 0)
(test-begin "vectors")
(test-group "vector->list"
  (test-equal "vector->list full" '(a b c) (vector->list #(a b c)))
  (test-equal "vector->list with start/end" '(b c) (vector->list #(a b c d e) 1 3)))
(test-group "vector-map"
  (test-equal "vector-map with two vectors" #(11 22 33) (vector-map + #(1 2 3) #(10 20 30))))
(set! %test-fail-count (test-runner-fail-count (test-runner-current)))
(test-end "vectors")
(if (> %test-fail-count 0) (exit 1))
