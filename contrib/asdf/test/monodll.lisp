(defpackage :test-asdf/monodll (:use :test-asdf/monodll-1)) ;; dummy, for package-inferred-system dependencies.

#+(and ecl (not clasp))
(ffi:clines "
extern int always_42();

int always_42()
{
        return 6*always_7();
}
")

#+clasp (leave-test "Not supported by CLASP yet" 0)

#+mkcl
(ffi:clines "
extern MKCL_DLLEXPORT int always_42(void);

int always_42(void)
{
        return 6*always_7();
}
")
