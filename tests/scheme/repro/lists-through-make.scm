;;; R7RS List compliance tests
(import (scheme base) (scheme process-context) (srfi 64))

(define %test-fail-count 0)
(test-begin "lists")

;; --- caar, cadr, cdar, cddr ---
(test-group "caar/cadr/cdar/cddr"
  (test-eqv "caar" 1 (caar '((1 2) 3)))
  (test-eqv "cadr" 2 (cadr '(1 2 3)))
  (test-equal "cdar" '(2) (cdar '((1 2) 3)))
  (test-equal "cddr" '(3) (cddr '(1 2 3))))

;; --- list-ref ---
(test-group "list-ref"
  (test-eq "list-ref index 0" 'a (list-ref '(a b c d) 0))
  (test-eq "list-ref index 2" 'c (list-ref '(a b c d) 2))
  (test-eq "list-ref index 3" 'd (list-ref '(a b c d) 3)))

;; --- list-tail ---
(test-group "list-tail"
  (test-equal "list-tail index 0" '(a b c d) (list-tail '(a b c d) 0))
  (test-equal "list-tail index 2" '(c d) (list-tail '(a b c d) 2))
  (test-equal "list-tail index 4" '() (list-tail '(a b c d) 4)))

;; --- list-set! ---
(test-group "list-set!"
  (test-equal "list-set! middle element"
    '(1 99 3)
    (let ((ls (list 1 2 3)))
      (list-set! ls 1 99)
      ls)))

;; --- list-copy ---
(test-group "list-copy"
  (test-equal "list-copy produces equal list"
    '(1 2 3)
    (list-copy (list 1 2 3)))
  (test-equal "list-copy is independent of original"
    '(1 2 3)
    (let ((original (list 1 2 3)))
      (let ((copy (list-copy original)))
        (list-set! copy 0 99)
        original)))
  (test-equal "list-copy mutation applies to copy"
    '(99 2 3)
    (let ((original (list 1 2 3)))
      (let ((copy (list-copy original)))
        (list-set! copy 0 99)
        copy))))

;; --- make-list ---
(test-group "make-list"
  (test-equal "make-list with fill" '(0 0 0) (make-list 3 0))
  (test-equal "make-list zero length" '() (make-list 0)))
(test-group "member"
  (test-equal "member found" '(3 4 5) (member 3 '(1 2 3 4 5))))
(test-end "lists")
