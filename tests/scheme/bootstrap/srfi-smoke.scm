;; Phase 9: portable SRFI smoke (partial)

(import (scheme base))
(import (srfi 1))
(import (srfi lists-1))
(import (srfi 35))
(import (srfi 64))

(check-equal "srfi-1 iota" '(0 1 2 3 4) (iota 5))
(check-eqv "srfi-261 alias import" 10 (first '(10 20)))
(check-assert "srfi-35 condition-type?" (condition-type? &condition))
(check-assert "srfi-64 import" (procedure? test-runner-null))
(check-equal "cond-expand srfi-1 feature" 'yes
             (cond-expand (srfi-1 'yes) (else 'no)))
(check-equal "cond-expand srfi-261 feature" 'yes
             (cond-expand (srfi-261 'yes) (else 'no)))
(check-equal "cond-expand srfi-64 feature" 'yes
             (cond-expand (srfi-64 'yes) (else 'no)))

(import (srfi 13))
(import (srfi 14))
(import (srfi 69))
(check-assert "srfi-13 string-prefix?" (string-prefix? "foo" "foobar"))
(check-assert "srfi-14 char-set-contains?" (char-set-contains? char-set:full #\a))
(check-eqv "srfi-69 hash table" 42
           (let ((ht (make-hash-table eq?)))
             (hash-table-set! ht 'answer 42)
             (hash-table-ref ht 'answer)))

(import (srfi 258))
(import (srfi 260))
(check-assert "srfi-258 uninterned symbol"
              (and (symbol? (string->uninterned-symbol "x"))
                   (not (symbol-interned? (string->uninterned-symbol "x")))))
(check-assert "srfi-260 generate-symbol"
              (symbol? (generate-symbol "test")))
(check-equal "srfi-13 string-unfold" "ABCDE"
             (string-unfold (lambda (x) (> x 4))
                            (lambda (x) (integer->char (+ x 65)))
                            (lambda (x) (+ x 1))
                            0))

;; Batch of SRFI-1/SRFI-13 primitives added by the Phase 4 import audit
;; (previously listed in library_builtin.c's export tables but never
;; implemented, so importing (srfi 1)/(srfi 13) never actually exposed them).
(check-equal "srfi-1 fold-right" '(1 2 3)
             (fold-right cons '() '(1 2 3)))
(check-equal "srfi-1 remove" '(1 3)
             (remove even? '(1 2 3 4)))
(check-eqv "srfi-1 find" 3
           (find odd? '(2 4 3 6)))
(check-assert "srfi-1 any" (any odd? '(2 4 5 6)))
(check-assert "srfi-1 every" (every odd? '(1 3 5)))
(check-equal "srfi-1 alist-cons" '((a . 1) (b . 2))
             (alist-cons 'a 1 '((b . 2))))
(check-equal "srfi-1 alist-delete" '((b . 2))
             (alist-delete 'a '((a . 1) (b . 2))))
(check-equal "srfi-1 delete" '(1 3)
             (delete 2 '(1 2 3)))
(check-equal "srfi-1 lset-adjoin" '(3 1 2)
             (lset-adjoin eqv? '(1 2) 1 3))
(check-assert "srfi-1 lset-union"
              (lset= eqv? '(1 2 3) (lset-union eqv? '(1 2) '(2 3))))
(check-equal "srfi-1 append-reverse" '(3 2 1 4 5)
             (append-reverse '(1 2 3) '(4 5)))

(check-eqv "srfi-13 string-every" #t (string-every char-alphabetic? "abc"))
(check-assert "srfi-13 string-any" (string-any char-numeric? "ab3"))
(check-eqv "srfi-13 string-count" 3 (string-count "banana" (lambda (c) (char=? c #\a))))
(check-equal "srfi-13 string-filter" "bnn"
             (string-filter (lambda (c) (not (char=? c #\a))) "banana"))
(check-equal "srfi-13 string-delete" "bnn"
             (string-delete (lambda (c) (char=? c #\a)) "banana"))

;; Newly-green portable library imports flipped by this audit pass.
(import (srfi 51))
(import (srfi 116))
(import (srfi 140))
(import (srfi 152))
(import (srfi 263))
(check-equal "srfi-116 ifold-right" '(1 2 3)
             (ifold-right ipair '() (ilist 1 2 3)))
(check-assert "srfi-263 root object exists" (procedure? *the-root-object*))
(check-eqv "srfi-152 string-count re-export" 3
           (string-count "banana" (lambda (c) (char=? c #\a))))
(check-assert "srfi-140 istring? re-export" (not (istring? "x")))

(check-finish)
