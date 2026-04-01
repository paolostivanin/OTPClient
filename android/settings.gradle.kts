pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

@Suppress("UnstableApiUsage")
dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "OTPClient"

include(":app")
include(":core:core-model")
include(":core:core-crypto")
include(":core:core-otp")
include(":core:core-database")
include(":core:core-importexport")
include(":feature:feature-unlock")
include(":feature:feature-tokenlist")
include(":feature:feature-addtoken")
include(":feature:feature-settings")
include(":feature:feature-importexport")
include(":feature:feature-dbmanager")
