(import (scheme base) (srfi 64))
(test-begin "v")
(test-group "vector->list"
  (test-equal "vector->list full" '(a b c) (vector->list #(a b c)))
  (test-equal "vector->list with start/end" '(b c) (vector->list #(a b c d e) 1 3)))
(test-end "v")
