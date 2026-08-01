(import (scheme base) (scheme process-context) (srfi 64))
(test-begin "lists")
(test-group "member"
  (test-equal "member found" '(3 4 5) (member 3 '(1 2 3 4 5)))
  (test-eqv "member not found" #f (member 6 '(1 2 3 4 5)))
  (test-equal "member with equal?" '((b) (c)) (member '(b) '((a) (b) (c)))))
(test-end "lists")
