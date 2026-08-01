;; Phase 7A: hash tables

(define ht (make-hash-table))
(check-assert "hash-table? true" (hash-table? ht))
(check-assert "hash-table? false" (not (hash-table? '(a . b))))

(hash-table-set! ht 'alpha 10)
(check-eqv "ref symbol key" 10 (hash-table-ref ht 'alpha))
(hash-table-set! ht 'alpha 11)
(check-eqv "overwrite symbol key" 11 (hash-table-ref ht 'alpha))
(check-eqv "size after overwrite" 1 (hash-table-size ht))

(hash-table-set! ht 42 'n)
(check-eq "ref fixnum key" 'n (hash-table-ref ht 42))
(check-eqv "size with fixnum key" 2 (hash-table-size ht))

(define str-key (make-string 3 #\k))
(string-set! str-key 1 #\e)
(string-set! str-key 2 #\y)
(hash-table-set! ht str-key 's)
(check-eq "ref string key same object" 's (hash-table-ref ht str-key))
(check-eqv "size with string key" 3 (hash-table-size ht))

(check-eqv "ref default value" 99 (hash-table-ref ht 'missing 99))
(check-eqv "ref default thunk" 77 (hash-table-ref ht 'missing (lambda () 77)))

(hash-table-delete! ht 'alpha)
(check-eqv "size after delete" 2 (hash-table-size ht))
(check-eqv "delete missing is no-op"
           2
           (begin
             (hash-table-delete! ht 'missing)
             (hash-table-size ht)))

;; eq? mode: distinct string objects remain distinct.
(define (fresh-a)
  (make-string 1 #\a))

(define ht-eq (make-hash-table eq?))
(define s1 (fresh-a))
(define s2 (fresh-a))
(hash-table-set! ht-eq s1 1)
(hash-table-set! ht-eq s2 2)
(check-eqv "eq? mode distinct strings" 2 (hash-table-size ht-eq))

;; eqv? mode: equal fixnums overwrite.
(define ht-eqv (make-hash-table eqv?))
(hash-table-set! ht-eqv 7 'a)
(hash-table-set! ht-eqv 7 'b)
(check-eqv "eqv? mode overwrite fixnum" 1 (hash-table-size ht-eqv))
(check-eq "eqv? mode ref fixnum" 'b (hash-table-ref ht-eqv 7))

(define ks (hash-table-keys ht))
(define vs (hash-table-values ht))
(check-eqv "keys length" 2 (length ks))
(check-eqv "values length" 2 (length vs))
(check-assert "keys contain 42" (memv 42 ks))
(check-assert "values contain n" (memq 'n vs))
(check-assert "values contain s" (memq 's vs))

;; walk/fold basic behavior.
(define ht-walk (make-hash-table))
(let loop ((i 0))
  (if (= i 30)
      #t
      (begin
        (hash-table-set! ht-walk i i)
        (loop (+ i 1)))))

(define walk-sum 0)
(hash-table-walk ht-walk
  (lambda (k v)
    (set! walk-sum (+ walk-sum v))))
(check-eqv "walk sums values" 435 walk-sum)

(check-eqv "fold sums values"
           435
           (hash-table-fold ht-walk
                            (lambda (k v acc) (+ acc v))
                            0))

;; Snapshot GC safety: callbacks clear/rehash table and allocate aggressively.
(define (burn n)
  (if (= n 0)
      'done
      (begin
        (cons n 'tmp)
        (burn (- n 1)))))

(define ht-gc (make-hash-table))
(let fill ((i 0))
  (if (= i 40)
      #t
      (begin
        (hash-table-set! ht-gc i (cons i 'value))
        (fill (+ i 1)))))

(define walked 0)
(hash-table-walk ht-gc
  (lambda (k v)
    (set! walked (+ walked 1))
    (for-each (lambda (kk) (hash-table-delete! ht-gc kk))
              (hash-table-keys ht-gc))
    (burn 80)
    (hash-table-set! ht-gc (+ k 1000) v)))
(check-eqv "walk snapshot survives mutation+gc" 40 walked)

(define ht-fold (make-hash-table))
(let fill2 ((i 0))
  (if (= i 35)
      #t
      (begin
        (hash-table-set! ht-fold i (cons i 'value))
        (fill2 (+ i 1)))))

(define folded-count
  (hash-table-fold ht-fold
                   (lambda (k v acc)
                     (for-each (lambda (kk) (hash-table-delete! ht-fold kk))
                               (hash-table-keys ht-fold))
                     (burn 80)
                     (+ acc 1))
                   0))
(check-eqv "fold snapshot survives mutation+gc" 35 folded-count)

(check-finish)
