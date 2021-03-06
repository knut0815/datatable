@Library('test-shared-library@1.2') _

import ai.h2o.ci.Utils

def utilsLib = new Utils()

properties([
    buildDiscarder(logRotator(artifactDaysToKeepStr: '', artifactNumToKeepStr: '', daysToKeepStr: '180', numToKeepStr: ''))
])

def X86_64_LINUX_DOCKER_IMAGE = 'docker.h2o.ai/opsh2oai/datatable-build-x86_64_linux'

def EXPECTED_SHAS = [
    files: [
        'Dockerfile.build': 'e01271264d0ec9f50b2707479f8689441fa89cb3',
        'Dockerfile-centos7.in': '4b54cdd262d7b75d854c48fde6fc5db83af01bb0',
    ],
    images: [
        (X86_64_LINUX_DOCKER_IMAGE): 'docker.h2o.ai/opsh2oai/datatable-build-x86_64_linux@sha256:0e09d9145fc1ae14ae29acebacd9d5dc17e5d7b913234eb291909982749abf24',
        'docker.h2o.ai/opsh2oai/datatable-build-x86_64_centos7': 'docker.h2o.ai/opsh2oai/datatable-build-x86_64_centos7@sha256:123d142f8e27feb9a0e3cde7e2e7fcf2587c74259f226c73f9c89ce21cd60026',
        'docker.h2o.ai/opsh2oai/datatable-build-ppc64le_centos7': 'e0d49e7a7e0e'
    ]
]

// Default mapping
def platformDefaults = [
        env      : [],
        pythonBin: 'python',
        coverage : true,

]

// Default mapping for CentOS environments
def centosDefaults = [
    env: ['LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/opt/h2oai/dai/python/lib/'],
    pullImage: true
]

def PPC64LE_PLATFORM = 'ppc64le_linux'
def PPC64LE_BUILD_CONF = platformDefaults + centosDefaults + [
    node: 'ibm-power',
    coverage: false,
    pullImage: false,
    dockerImage: 'docker.h2o.ai/opsh2oai/datatable-build-ppc64le_centos7'
]

// Build platforms parameters
def BUILD_MATRIX = [
    // Linux build definition
    x86_64_linux: platformDefaults + [
        pullImage: true,
        node: 'docker && linux',
        dockerImage: X86_64_LINUX_DOCKER_IMAGE
    ],
    // CentOS 7 build definition
    x86_64_centos7: platformDefaults + centosDefaults + [
        node: 'docker && linux',
        coverage: false,
        dockerImage: 'docker.h2o.ai/opsh2oai/datatable-build-x86_64_centos7'
    ],
    // OSX
    x86_64_macos: platformDefaults + [
        node: 'osx',
        env: [
            "LLVM4=/usr/local/opt/llvm",
            "CI_EXTRA_COMPILE_ARGS=-DDISABLE_CLOCK_REALTIME"
        ],
    ]
]

if (!isPrJob()) {
    BUILD_MATRIX[PPC64LE_PLATFORM] = PPC64LE_BUILD_CONF
}

// Virtualenv used for build and coverage
def DEFAULT_PY36_ENV = 'datatable-py36-pandas-numpy'
def DEFAULT_PY35_ENV = 'datatable-py35-pandas-numpy'
// Virtualenvs used for PR testing
PR_TEST_ENVIRONMENTS = [
    DEFAULT_PY35_ENV,
    DEFAULT_PY36_ENV
]
// Virtualenvs used for nightly testing
NIGHTLY_TEST_ENVIRONMENTS = PR_TEST_ENVIRONMENTS + [
    'datatable-py36',
    'datatable-py36-numpy'
]

// Computed version suffix
def CI_VERSION_SUFFIX = utilsLib.getCiVersionSuffix()

// Global project build trigger filled in init stage
def project

// Needs invocation of larger tests
def needsLargerTest

// Paths should be absolute
SOURCE_DIR = "/home/0xdiag"
TARGET_DIR = "/tmp/pydatatable_large_data"

// Data map for linking into container
def linkMap = [ 
        "Data"      : "h2oai-benchmarks/Data",
        "smalldata" : "h2o-3/smalldata",
        "bigdata"   : "h2o-3/bigdata",
        "fread"     : "h2o-3/fread",
]

def dockerArgs = createDockerArgs(linkMap, SOURCE_DIR, TARGET_DIR)

def stages = [:]
BUILD_MATRIX.each {platform, config ->
    stages[platform] = {
        node(config.node) {
            final def buildDir = 'build'

            cleanWs()

            stage("Build ${platform}") {
                dir (buildDir) {
                    unstash 'datatable-sources'
                }
                if (config.dockerImage && config.pullImage) {
                    withCredentials([usernamePassword(credentialsId: 'docker.h2o.ai', usernameVariable: 'REGISTRY_USERNAME', passwordVariable: 'REGISTRY_PASSWORD')]) {
                        // by default, the image should be pulled
                        def pullImage = true
                        // First check that the image is present
                        def imagePresent = sh(script: "docker inspect ${config.dockerImage}", returnStatus: true) == 0
                        if (imagePresent) {
                            echo "${config.dockerImage} present on host, checking versions..."
                            // check that the image has expected SHA
                            def expectedVersion = EXPECTED_SHAS.images[config.dockerImage]
                            def currentVersion = sh(script: "docker inspect --format=\'{{index .RepoDigests 0}}\' ${config.dockerImage}", returnStdout: true).trim()
                            echo "current image version: ${currentVersion}"
                            echo "expected image version: ${expectedVersion}"
                            pullImage = currentVersion != expectedVersion
                        }
                        if (pullImage) {
                            sh """
                                docker login -u $REGISTRY_USERNAME -p $REGISTRY_PASSWORD docker.h2o.ai
                                docker pull ${config.dockerImage}
                            """
                        }
                    }
                }
                // Specify build Closure
                def buildClosure = {
                    project.buildAll(buildDir, platform, CI_VERSION_SUFFIX, getVenvActivationCmd(platform, DEFAULT_PY36_ENV),getVenvActivationCmd(platform, DEFAULT_PY35_ENV), BUILD_MATRIX[platform]['env'])
                }
                callInEnv(config.dockerImage, dockerArgs, buildClosure)
            }

            if (config.coverage) {
                final def coverageDir = "coverage-${platform}"
                dir(coverageDir) {
                    unstash 'datatable-sources'
                }
                stage("Coverage ${platform}") {
                    def coverageClosure = {
                        project.coverage(coverageDir, platform, getVenvActivationCmd(platform, DEFAULT_PY36_ENV), BUILD_MATRIX[platform]['env'], false, TARGET_DIR)
                    }
                    callInEnv(config.dockerImage, dockerArgs, coverageClosure)
                }
            } else {
                stage("Coverage ${platform} - SKIPPED") {
                    echo "SKIPPED"
                }
            }

            boolean failure = false
            getRelevantTestEnvs().each {testEnv ->
                final def testDir = "test-${platform}-${testEnv}"
                try {
                    stage("Test ${platform} with ${testEnv}") {
                        dir(testDir) {
                            unstash 'datatable-sources'
                        }
                        def testClosure = {
                            project.test(testDir, platform, getVenvActivationCmd(platform, testEnv), BUILD_MATRIX[platform]['env'], needsLargerTest, TARGET_DIR)
                        }
                        callInEnv(config.dockerImage, dockerArgs, testClosure)
                    }
                } catch (e) {
                    failure = true
                    StringWriter sw = new StringWriter()
                    e.printStackTrace(new PrintWriter(sw))
                    echo sw.toString()
                }
            }
            if (failure) {
                error 'There were one or more test failures. Please check the stacktrace above.'
            }
        }
    }
}

ansiColor('xterm') {
    timestamps {
        if (isPrJob()) {
            cancelPreviousBuilds()
        }
        node('linux') {
            stage('Init') {
                checkout scm
                buildInfo(env.BRANCH_NAME, false)
                project = load 'ci/default.groovy'
                needsLargerTest = isModified("(py_)?fread\\..*|__version__\\.py")
                if (needsLargerTest) {
                    manager.addBadge("warning.gif", "Large tests required")
                }
                stash 'datatable-sources'
                docker.image(X86_64_LINUX_DOCKER_IMAGE).inside {
                    def dockerfileSHAsString = ""
                    EXPECTED_SHAS.files.each {filename, sha ->
                        dockerfileSHAsString += "${sha}\t${filename}\n"
                    }
                    try {
                        sh """
                            echo "${dockerfileSHAsString}" > dockerfiles.sha
                            sha1sum -c dockerfiles.sha
                            rm -f dockerfiles.sha
                        """
                    } catch (e) {
                        error "Dockerfiles do not have expected checksums. Please make sure, you have built the " +
                                "new images using the Jenkins pipeline and that you have changed the required " +
                                "fields in this pipeline."
                        throw e
                    }
                }
            }
        }

        parallel stages

        // Publish into S3 all snapshots versions
        if (doPublish()) {
            node('docker && !mr-0xc8') {
                def publishS3Dir = 'publish-s3'

                stage('Publish snapshot to S3') {
                    cleanWs()
                    dumpInfo()

                    project.pullFilesFromArch('build/dist/**/*.whl', "${publishS3Dir}/dist")
                    unstash 'VERSION'
                    sh "echo 'Stashed files:' && cd ${publishS3Dir} && find dist"

                    def versionText = utilsLib.getCommandOutput("cat build/dist/VERSION.txt")

                    BUILD_MATRIX.each { platform, config ->
                        uploadToS3("${publishS3Dir}/dist/*${project.getWheelPlatformName(platform)}*", versionText, project.getS3PlatformName(platform))
                    }
                }
            }
        }
    }
}

def uploadToS3(artifact, versionText, platformDir) {
    s3upDocker {
        localArtifact = artifact
        artifactId = 'pydatatable'
        version = versionText
        keepPrivate = false
        platform = platformDir
        isRelease = true
    }
}

def getVenvActivationCmd(final platform, final venvName) {
    switch (platform) {
        case 'x86_64_linux':
            return ". /envs/${venvName}/bin/activate"
        case 'x86_64_centos7':
            return ". activate ${venvName}"
        case 'x86_64_macos':
            return ". /Users/jenkins/anaconda/bin/activate ${venvName}"
        case 'ppc64le_linux':
            return ". activate ${venvName}"
    }
}
def isPrJob() {
    return env.CHANGE_BRANCH != null && env.CHANGE_BRANCH != ''
}

def getRelevantTestEnvs() {
    if (isPrJob()) {
        return PR_TEST_ENVIRONMENTS
    }
    return NIGHTLY_TEST_ENVIRONMENTS
}

def isModified(pattern) {
    def fList = buildInfo.get().getChangedFiles().join('\n')
    out = sh(script: "echo '${fList}' | xargs basename | egrep -e '${pattern}' | wc -l", returnStdout: true).trim()
    return !(out.isEmpty() || out == "0")
}

def createDockerArgs(linkMap, sourceDir, targetDir) {
    def out = ""
    linkMap.each { key, value ->
        out += "-v ${sourceDir}/${key}:${targetDir}/${value} "
    }
    return out
}

def linkFolders(sourceDir, targetDir) {
    sh """
        mkdir -p ${targetDir}

        mkdir -p ${targetDir}/h2oai-benchmarks
        ln -sf ${sourceDir}/Data ${targetDir}/h2oai-benchmarks

        mkdir -p ${targetDir}/h2o-3
        ln -sf ${sourceDir}/smalldata ${targetDir}/h2o-3
        ln -sf ${sourceDir}/bigdata ${targetDir}/h2o-3
        ln -sf ${sourceDir}/fread ${targetDir}/h2o-3
        find ${targetDir}
    """
}

/**
 * If dockerImage is specified, than execute the body inside container, otherwise execute that on host
 * @param dockerImage docker image to use, or null to run on host
 * @param body body (Closure) to execute
 */
def callInEnv(dockerImage, dockerArgs, body) {
    // Call closure inside dockerImage or directly on host
    if (dockerImage != null) {
        docker.image(dockerImage).inside(dockerArgs) {
            body()
        }
    } else {
        linkFolders(SOURCE_DIR, TARGET_DIR)
        body()
    }
}

def doPublish() {
    return env.BRANCH_NAME == 'master'
}