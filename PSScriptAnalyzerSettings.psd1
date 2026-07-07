@{
    IncludeDefaultRules = $true
    ExcludeRules = @(
        # These scripts are operator-facing entrypoints; progress output is
        # intentional and easier to follow than pipeline output.
        'PSAvoidUsingWriteHost',
        # Repository files are UTF-8 without BOM. Keep the repo-wide encoding
        # convention instead of converting selected PowerShell files to BOM.
        'PSUseBOMForUnicodeEncodedFile'
    )
}
