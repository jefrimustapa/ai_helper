/* Unit tests for root_validate() (COH-517). Build+run: python tests/test_roots.py
 * Includes ai-helper.c directly (wWinMain renamed) so the real function is tested. */
#define wWinMain ai_wWinMain
#include "../ai-helper.c"
#undef wWinMain

static int fails;
static void check(const char *name, int cond)
{
    printf("%s %s\n", cond ? "PASS" : "FAIL", name);
    if (!cond) fails++;
}

static wchar_t ex[MAX_ROOTS + 2][MAX_PATH];

static int ok(const wchar_t *p, int n)            /* 1 = accepted */
{
    wchar_t c[MAX_PATH];
    return root_validate(p, c, ex, n) == NULL;
}

int wmain(int argc, wchar_t **argv)
{
    wchar_t base[MAX_PATH], d1[MAX_PATH], d2[MAX_PATH], f[MAX_PATH], longp[MAX_PATH + 40], c[MAX_PATH];
    if (argc < 2) { puts("usage: test_roots <scratch dir>"); return 2; }
    wcscpy(base, argv[1]);
    swprintf(d1, MAX_PATH, L"%ls\\one", base);
    swprintf(d2, MAX_PATH, L"%ls\\two", base);
    swprintf(f, MAX_PATH, L"%ls\\file.txt", base);
    CreateDirectoryW(base, NULL); CreateDirectoryW(d1, NULL); CreateDirectoryW(d2, NULL);
    HANDLE h = CreateFileW(f, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL); CloseHandle(h);

    /* (a) absolute / UNC / relative / empty */
    check("a pass: absolute dir accepted", ok(d1, 0));
    check("a fail: empty", !ok(L"", 0));
    check("a fail: NULL", !ok(NULL, 0));
    check("a fail: UNC", !ok(L"\\\\server\\share\\x", 0));
    check("a fail: UNC admin share", !ok(L"\\\\localhost\\c$\\Windows", 0));
    check("a fail: relative", !ok(L"one", 0));
    check("a fail: drive-relative 'C:'", !ok(L"C:", 0));
    check("a fail: drive-relative 'C:foo'", !ok(L"C:foo", 0));
    check("a fail: traversal path resolved to... '..' to nonexistent", !ok(L"D:\\no_such_dir_zz\\..\\x", 0));
    /* (b) exists + folder */
    check("b fail: nonexistent", !ok(L"D:\\no_such_dir_zz_517", 0));
    check("b fail: file not folder", !ok(f, 0));
    /* (c) root drive */
    check("c fail: C:\\", !ok(L"C:\\", 0));
    check("c fail: c:\\ lowercase", !ok(L"c:\\", 0));
    check("c fail: D:\\", !ok(L"D:\\", 0));
    check("c pass: sub folder of drive ok", ok(d1, 0));
    wchar_t drv[4] = { d1[0], L':', L'\\', 0 };
    check("c fail: root of scratch drive", !ok(drv, 0));
    /* canonical form: trailing backslash and mixed case */
    swprintf(c, MAX_PATH, L"%ls\\", d1);
    check("trailing backslash accepted", ok(c, 0));
    {
        wchar_t out[MAX_PATH];
        root_validate(c, out, ex, 0);
        check("canonical form has no trailing backslash", out[wcslen(out) - 1] != L'\\');
    }
    /* (d) duplicates */
    wcscpy(ex[0], d1);
    check("d fail: duplicate", !ok(d1, 1));
    {
        wchar_t up[MAX_PATH]; wcscpy(up, d1); _wcsupr(up);
        check("d fail: duplicate differing in case", !ok(up, 1));
        swprintf(c, MAX_PATH, L"%ls\\", d1);
        check("d fail: duplicate with trailing backslash", !ok(c, 1));
    }
    check("d pass: different folder accepted next to existing", ok(d2, 1));
    check("d pass: 0 existing, d1 ok (empty list)", ok(d1, 0));
    /* (e) limit 16: 15 existing -> 16th ok; 16 existing -> 17th rejected */
    for (int i = 0; i < MAX_ROOTS; i++) swprintf(ex[i], MAX_PATH, L"%ls\\other%d", base, i);
    check("e pass: 15 existing -> 16th accepted", ok(d1, MAX_ROOTS - 1));
    check("e fail: 16 existing -> 17th rejected", !ok(d1, MAX_ROOTS));
    /* path length boundary */
    wcscpy(longp, base);
    while (wcslen(longp) < MAX_PATH - 1) wcscat(longp, L"\\aaaaaaaaaaaaaaaa");
    check("path >= MAX_PATH rejected", !ok(longp, 0));
    swprintf(c, MAX_PATH, L"%ls\\one\\..\\two", base);
    check("'..' inside path accepted only as canonical existing dir (maps to two)", ok(c, 0));
    {
        wchar_t out[MAX_PATH]; wcscpy(ex[0], d2);
        check("'..' alias of existing is flagged duplicate", root_validate(c, out, ex, 1) != NULL);
    }
    printf("%s\n", fails ? "FAILED" : "ALL PASSED");
    return fails ? 1 : 0;
}
